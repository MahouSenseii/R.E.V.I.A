"""Loopback-only Qwen3-TTS worker used by the C++ Revia runtime.

Models load lazily and only one model is resident at a time. This keeps voice
design and voice cloning functional on machines that cannot hold both models.
"""

from __future__ import annotations

import argparse
import gc
import io
import json
import os
import sys
import threading
import traceback
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any


REFLEX_CACHE_TEXT = frozenset({
    "Okay.", "Stopped.", "I'm here.", "Yeah?", "Hm?", "What?", "Mm?",
    "Hold on—what?", "I heard you the first time.",
})



def _is_context_fatal(exception: BaseException) -> bool:
    """True when the CUDA context cannot serve another request in this process.

    A device-side assert or a sticky CUDA error leaves every subsequent kernel launch
    failing with the same text. Restarting is the only recovery.
    """
    text = str(exception).lower()
    return (
        "device-side assert" in text
        or "cuda error" in text
        or "unspecified launch failure" in text
        or "illegal memory access" in text
    )


def _reports_bf16(torch: Any) -> bool:
    """torch.cuda.is_bf16_supported() for the current device, emulation excluded.

    including_emulation was added in PyTorch 2.4 and defaults to True, which is the
    permissive answer. Older builds do not accept the argument at all, so fall back
    to the plain call - the compute-capability check above is what actually protects
    Turing either way.
    """
    try:
        return bool(torch.cuda.is_bf16_supported(including_emulation=False))
    except TypeError:
        return bool(torch.cuda.is_bf16_supported())


def _enable_ampere_tf32(torch: Any, device: str) -> bool:
    """Use TensorFloat-32 for residual FP32 inference work on Ampere or newer.

    The conversational clone model stays in BF16. This only accelerates FP32 matrix
    operations left in the tokenizer/decoder path, and is deliberately capability-gated:
    older CUDA devices and CPU workers keep their existing numerical path.
    """
    if not device.startswith("cuda:"):
        return False
    try:
        index = int(device.split(":", 1)[1])
        major, _ = torch.cuda.get_device_capability(index)
        if major < 8:
            return False
        torch.set_float32_matmul_precision("high")
        torch.backends.cuda.matmul.allow_tf32 = True
        torch.backends.cudnn.allow_tf32 = True
        return True
    except (AttributeError, RuntimeError, TypeError, ValueError):
        # A backend that cannot expose or set TF32 is still a valid inference backend.
        return False


class QwenRuntime:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.lock = threading.Lock()
        self.model: Any | None = None
        self.model_kind = ""
        self.model_name = ""
        self.device = "cpu"
        self.device_name = "CPU"
        self.dtype_name = "float32"
        self.attention_backend = "auto"
        self.cuda_math_mode = "default"
        self.loaded_at = 0.0
        self.last_used = 0.0
        # Which inference path this worker actually runs, decided at load time and
        # reported on every request. "standard" is stock Hugging Face generation.
        self.backend = "standard"
        self.cuda_graph = False
        self.low_latency_detail = ""
        self.clone_prompts: dict[tuple[str, int, int, str], Any] = {}
        self.reflex_audio_cache: dict[tuple[Any, ...], tuple[bytes, int, float]] = {}

    @staticmethod
    def _cuda_candidate(torch: Any, index: int) -> tuple[str, Any, str, int]:
        properties = torch.cuda.get_device_properties(index)
        with torch.cuda.device(index):
            free_bytes, total_bytes = torch.cuda.mem_get_info()
            reported_bf16 = _reports_bf16(torch)

        # RTX 20-series/Turing supports FP16 but not native BF16. Selecting BF16 for
        # every CUDA adapter made a perfectly useful secondary 2070 fail at runtime.
        #
        # torch.cuda.is_bf16_supported() alone is NOT a sufficient gate: since PyTorch
        # 2.4 it defaults to including_emulation=True and answers True on Turing, so
        # the model loaded as bfloat16 on a 2070 SUPER and every synthesis died with
        # "GET was unable to find an engine to execute this computation" - the kernel
        # for that dtype does not exist on sm_75, which surfaces at the first matmul
        # rather than at load time.
        #
        # Compute capability is the authoritative gate. Native BF16 starts at Ampere
        # (sm_80). Both signals must agree before BF16 is chosen; either one alone can
        # be wrong in the permissive direction, never in the restrictive one.
        major, minor = torch.cuda.get_device_capability(index)
        native_bf16 = major >= 8
        bf16_supported = native_bf16 and reported_bf16

        # The fallback is FLOAT32, not float16, and that is deliberate.
        #
        # FP16 has the same mantissa width as BF16 but a far smaller exponent range
        # (max ~65504 against ~3.4e38). This model relies on BF16's range: run in FP16
        # on a 2070 SUPER and the logits overflow to inf, softmax collapses to all
        # zeros, and sampling dies inside torch.multinomial with
        #   TensorCompare.cu:112 Assertion `input[0] != 0` failed
        # reported as "CUDA error: device-side assert triggered" - a message that says
        # nothing about dtype and costs an afternoon to trace back to one.
        #
        # FP32 is slower and doubles the weights (a 0.6B model is ~2.4 GB, which fits
        # the 8 GB card with room to spare), but it is correct on every architecture.
        # Speed is worth trading for a voice that actually produces audio.
        dtype = torch.bfloat16 if bf16_supported else torch.float32
        dtype_name = "bfloat16" if bf16_supported else "float32"
        free_mib = free_bytes // (1024 * 1024)
        total_mib = total_bytes // (1024 * 1024)
        # sm_XX is in the log line on purpose: when a dtype problem does appear, the
        # architecture is the first thing worth knowing and the slowest thing to guess.
        detail = (
            f"{properties.name}, sm_{major}{minor}, "
            f"{free_mib}/{total_mib} MiB free, {dtype_name}"
        )
        return f"cuda:{index}", dtype, detail, free_mib

    def _select_device(self) -> tuple[str, Any, str]:
        import torch

        requested = self.args.device.lower()
        if requested == "cpu":
            self.device_name = "CPU"
            self.dtype_name = "float32"
            return "cpu", torch.float32, "CPU selected by configuration"
        if requested.startswith("cuda") and torch.cuda.is_available():
            try:
                index = int(requested.split(":", 1)[1]) if ":" in requested else 0
                if index < 0 or index >= torch.cuda.device_count():
                    raise ValueError(f"CUDA device {index} does not exist")
                device, dtype, detail, free_mib = self._cuda_candidate(torch, index)
                if free_mib < self.args.minimum_free_vram_mib:
                    self.device_name = "CPU"
                    self.dtype_name = "float32"
                    return "cpu", torch.float32, (
                        f"CPU selected because {device} has only {free_mib} MiB free; "
                        f"{self.args.minimum_free_vram_mib} MiB is required"
                    )
                self.device_name = torch.cuda.get_device_name(index)
                self.dtype_name = "bfloat16" if dtype == torch.bfloat16 else "float32"
                return device, dtype, f"CUDA selected by resource plan: {detail}"
            except (TypeError, ValueError, RuntimeError) as exception:
                self.device_name = "CPU"
                self.dtype_name = "float32"
                return "cpu", torch.float32, f"Invalid CUDA assignment; using CPU: {exception}"
        if requested not in ("auto", "cuda"):
            self.device_name = "CPU"
            self.dtype_name = "float32"
            return "cpu", torch.float32, f"Unknown device '{requested}'; using CPU"
        if torch.cuda.is_available():
            candidates = [self._cuda_candidate(torch, index)
                          for index in range(torch.cuda.device_count())]
            candidates.sort(key=lambda candidate: candidate[3], reverse=True)
            device, dtype, detail, free_mib = candidates[0]
            if free_mib >= self.args.minimum_free_vram_mib:
                index = int(device.split(":", 1)[1])
                self.device_name = torch.cuda.get_device_name(index)
                self.dtype_name = "bfloat16" if dtype == torch.bfloat16 else "float32"
                return device, dtype, f"best free CUDA device selected: {detail}"
            self.device_name = "CPU"
            self.dtype_name = "float32"
            return "cpu", torch.float32, (
                f"CPU selected because the freest GPU has only {free_mib} MiB free; "
                f"{self.args.minimum_free_vram_mib} MiB is required"
            )
        self.device_name = "CPU"
        self.dtype_name = "float32"
        return "cpu", torch.float32, "CPU selected because CUDA is unavailable"

    def _unload(self) -> None:
        self.model = None
        self.model_kind = ""
        self.model_name = ""
        self.clone_prompts.clear()
        self.reflex_audio_cache.clear()
        gc.collect()
        try:
            import torch

            if torch.cuda.is_available() and self.device.startswith("cuda:"):
                with torch.cuda.device(int(self.device.split(":", 1)[1])):
                    torch.cuda.empty_cache()
        except Exception:
            pass

    def _load(self, kind: str) -> Any:
        model_name = self.args.design_model if kind == "design" else self.args.clone_model
        if self.model is not None and self.model_kind == kind and self.model_name == model_name:
            return self.model

        self._unload()
        import torch
        from qwen_tts import Qwen3TTSModel

        torch.set_num_threads(max(1, self.args.cpu_threads))
        try:
            torch.set_num_interop_threads(1)
        except RuntimeError:
            # PyTorch permits this only before inter-op work begins. The intra-op cap
            # above remains effective if a later model swap reaches this path.
            pass

        self.device, dtype, device_reason = self._select_device()
        self.cuda_math_mode = (
            "tf32" if _enable_ampere_tf32(torch, self.device) else "default"
        )
        selected_attention = self.args.attention_backend
        if selected_attention == "adaptive":
            selected_attention = "sdpa" if dtype == torch.bfloat16 else "auto"
        self.attention_backend = selected_attention
        print(
            f"[Qwen3-TTS] Loading {kind} model {model_name} on {self.device}: "
            f"{device_reason}; CUDA math={self.cuda_math_mode}",
            flush=True,
        )
        load_options: dict[str, Any] = {
            "device_map": self.device,
            "dtype": dtype,
        }
        if selected_attention != "auto":
            load_options["attn_implementation"] = selected_attention
        try:
            self.model = Qwen3TTSModel.from_pretrained(
                model_name,
                **load_options,
            )
        except Exception:
            if self.device == "cpu":
                raise
            print("[Qwen3-TTS] CUDA model load failed; retrying on CPU.", flush=True)
            self._unload()
            self.device = "cpu"
            self.device_name = "CPU"
            self.dtype_name = "float32"
            self.cuda_math_mode = "default"
            selected_attention = "auto" if self.args.attention_backend == "adaptive" else self.args.attention_backend
            self.attention_backend = selected_attention
            load_options = {"device_map": "cpu", "dtype": torch.float32}
            if selected_attention != "auto":
                load_options["attn_implementation"] = selected_attention
            self.model = Qwen3TTSModel.from_pretrained(
                model_name,
                **load_options,
            )
        self.model_kind = kind
        self.model_name = model_name
        self.loaded_at = time.time()
        self.last_used = self.loaded_at

        self.backend = "standard"
        self.cuda_graph = False
        self.low_latency_detail = ""
        self.low_latency_direct = None
        # Only the clone model carries conversation. The design model is used once to
        # build a voice and gains nothing from a path tuned for repeated short phrases.
        if getattr(self.args, "low_latency", False) and kind == "clone":
            try:
                import qwen_lowlatency

                direct = qwen_lowlatency.install(
                    self.model,
                    instrument=False,
                    use_cuda_graph=bool(getattr(self.args, "cuda_graph", False)),
                    use_talker_graph=bool(getattr(self.args, "talker_graph", False)),
                )
                self.backend = "low_latency"
                # Held so the reported graph state is the real one. Capture happens on
                # the first eligible phrase, not here, so a flag latched at load time
                # would claim a graph that may never exist.
                self.low_latency_direct = direct
                stages = []
                if direct.graph is not None:
                    stages.append("predictor graph")
                if getattr(direct, "talker_graph", None) is not None:
                    stages.append("talker graph")
                self.low_latency_detail = (
                    ", ".join(stages) if stages else "eager direct loop")
                print("[Qwen3-TTS] Inference backend: " + self.backend
                      + " (" + self.low_latency_detail + ")", flush=True)
            except Exception as exception:  # noqa: BLE001 - never block speech
                self.backend = "standard"
                self.cuda_graph = False
                self.low_latency_detail = "unavailable: " + str(exception)
                print("[Qwen3-TTS] Low-latency path " + self.low_latency_detail
                      + "; using stock generation.", flush=True)
        return self.model


    def _talker_graph_active(self) -> bool:
        """Whether the talker decode step is being replayed from a graph right now."""
        direct = getattr(self, "low_latency_direct", None)
        talker = getattr(direct, "talker_graph", None) if direct is not None else None
        return bool(talker is not None and talker.graph is not None)

    def _graph_active(self) -> bool:
        """Whether a CUDA graph is actually carrying phrases right now.

        Asked per request rather than latched at load, because capture is deferred to
        the first eligible phrase and can still fail there. A flag set optimistically at
        startup would report a graph that never existed.
        """
        direct = getattr(self, "low_latency_direct", None)
        graph = getattr(direct, "graph", None) if direct is not None else None
        return bool(graph is not None and graph.graph is not None)

    def _backend_state(self) -> dict[str, Any]:
        """What was asked for, what was installed, and what is replaying right now.

        Three different facts, reported separately because they diverge in ways that
        matter and a single flag would have to lie about one of them. Capture is
        deferred to the first eligible phrase, so a worker can be on the low-latency
        path with no graph captured yet and be perfectly healthy. A worker whose
        install raised is on stock generation, and no amount of configuration makes
        that untrue. Collapsing the three is exactly how a session claims a graph it
        never had -- which is the mistake this reporting exists to prevent.
        """
        requested = bool(getattr(self.args, "low_latency", False))
        # Installation is attempted when the clone model loads, so before any load
        # there is nothing to report and "not installed" would be read as "failed".
        # A worker polled at startup is in exactly that state, and the distinction is
        # the difference between waiting and investigating.
        if not requested:
            state = "off"
        elif self.backend == "low_latency":
            state = "installed"
        elif self.model is None:
            state = "not-loaded"
        else:
            state = "unavailable"
        return {
            "backend": self.backend,
            "low_latency_requested": requested,
            "low_latency_installed": self.backend == "low_latency",
            "low_latency_state": state,
            "cuda_graph_requested": bool(getattr(self.args, "cuda_graph", False)),
            "talker_graph_requested": bool(getattr(self.args, "talker_graph", False)),
            "cuda_graph": self._graph_active(),
            "talker_graph": self._talker_graph_active(),
            "low_latency_detail": self.low_latency_detail,
        }

    def _gpu_reading(self) -> dict[str, Any]:
        """VRAM and utilisation for the device this worker actually holds.

        Reported per request rather than once at load because the number that explains a
        slow utterance is the one from that utterance: another process taking the card is
        invisible in a figure captured at startup. Utilisation is best-effort -- NVML is
        not always present, and a missing reading is reported as unknown rather than as
        zero, which would read as an idle card.
        """
        reading: dict[str, Any] = {
            "vram_used_mib": -1,
            "vram_total_mib": -1,
            "gpu_utilization": -1,
        }
        if not self.device.startswith("cuda:"):
            return reading
        index = int(self.device.split(":", 1)[1])
        try:
            import torch

            free_bytes, total_bytes = torch.cuda.mem_get_info(index)
            reading["vram_used_mib"] = (total_bytes - free_bytes) // (1024 * 1024)
            reading["vram_total_mib"] = total_bytes // (1024 * 1024)
        except Exception:
            pass
        try:
            import pynvml

            pynvml.nvmlInit()
            handle = pynvml.nvmlDeviceGetHandleByIndex(index)
            reading["gpu_utilization"] = int(
                pynvml.nvmlDeviceGetUtilizationRates(handle).gpu)
        except Exception:
            pass
        return reading

    @staticmethod
    def _prompt_key(reference_audio: Path, reference_text: str) -> tuple[str, int, int, str]:
        metadata = reference_audio.stat()
        return (str(reference_audio), metadata.st_size, metadata.st_mtime_ns, reference_text)

    def _voice_prompt(
        self,
        model: Any,
        reference_audio: Path,
        reference_text: str,
    ) -> tuple[Any, bool]:
        prompt_key = self._prompt_key(reference_audio, reference_text)
        voice_clone_prompt = self.clone_prompts.get(prompt_key)
        cache_hit = voice_clone_prompt is not None
        if voice_clone_prompt is None:
            voice_clone_prompt = model.create_voice_clone_prompt(
                ref_audio=str(reference_audio),
                ref_text=reference_text,
                x_vector_only_mode=False,
            )
            self.clone_prompts[prompt_key] = voice_clone_prompt
        return voice_clone_prompt, cache_hit

    @staticmethod
    def _validated_output(raw_path: str) -> Path:
        output = Path(raw_path).expanduser().resolve()
        if output.suffix.lower() != ".wav":
            raise ValueError("Voice output must use the .wav extension.")
        output.parent.mkdir(parents=True, exist_ok=True)
        return output

    @staticmethod
    def _text(value: Any, name: str, maximum: int) -> str:
        if not isinstance(value, str) or not value.strip():
            raise ValueError(f"{name} is required.")
        value = value.strip()
        if len(value) > maximum:
            raise ValueError(f"{name} exceeds {maximum} characters.")
        return value

    def design(self, request: dict[str, Any]) -> dict[str, Any]:
        with self.lock:
            started = time.perf_counter()
            text = self._text(request.get("text"), "Reference text", 600)
            description = self._text(request.get("description"), "Voice description", 1000)
            language = self._text(request.get("language", "English"), "Language", 40)
            output = self._validated_output(self._text(request.get("output_path"), "Output path", 2048))
            model = self._load("design")
            import soundfile as sf

            waveforms, sample_rate = model.generate_voice_design(
                text=text,
                language=language,
                instruct=description,
            )
            sf.write(output, waveforms[0], sample_rate)
            elapsed = (time.perf_counter() - started) * 1000.0
            return {
                "succeeded": True,
                "message": f"Voice reference created with {self.model_name} on {self.device}.",
                "output_path": str(output),
                "elapsed_ms": elapsed,
                "device": self.device,
                "device_name": self.device_name,
                "dtype": self.dtype_name,
            }

    def vocalizations(self, request: dict[str, Any]) -> dict[str, Any]:
        """Renders the nonverbal clip bank for one voice preset, once.

        Why this is a batch job at preset-creation time rather than a runtime call:
        a laugh that arrives a second and a half after the joke is not a laugh, it is a
        machine catching up. Playing a file that already exists is the only way the
        timing works, and it also keeps every vocalization on the VoiceDesign model,
        which is the only one that accepts a style instruction -- generate_voice_clone
        has no instruct parameter at all.

        Each clip is rendered from a short carrier phrase plus an instruction describing
        the SOUND. Asking for "a laugh" makes the model read the word; asking for the
        sound produces the sound.
        """
        with self.lock:
            started = time.perf_counter()
            directory = Path(self._text(request.get("directory"), "Output directory", 2048))
            language = self._text(request.get("language", "English"), "Language", 40)
            requested = request.get("kinds")
            if not isinstance(requested, list) or not requested:
                raise ValueError("At least one vocalization kind is required.")

            directory.mkdir(parents=True, exist_ok=True)
            model = self._load("design")
            import soundfile as sf

            written: list[str] = []
            failed: list[dict[str, str]] = []
            for entry in requested:
                if not isinstance(entry, dict):
                    continue
                kind = self._text(entry.get("kind"), "Vocalization kind", 40)
                instruct = self._text(entry.get("instruct"), "Style instruction", 1000)
                carrier = self._text(entry.get("carrier", "Ha."), "Carrier text", 200)
                variants = int(entry.get("variants", 2))
                variants = max(1, min(variants, 8))

                for index in range(1, variants + 1):
                    output = directory / f"{kind}-{index}.wav"
                    try:
                        # The instruction is varied per index so the variants differ.
                        # Identical inputs would give near-identical audio, and two
                        # indistinguishable clips are worse than one -- rotation would
                        # promise variety it cannot deliver.
                        shaped = instruct if index == 1 else f"{instruct} Slightly different delivery, variant {index}."
                        waveforms, sample_rate = model.generate_voice_design(
                            text=carrier,
                            language=language,
                            instruct=shaped,
                        )
                        sf.write(output, waveforms[0], sample_rate)
                        written.append(str(output))
                    except Exception as exception:  # noqa: BLE001 - reported, not raised
                        # One bad clip must not cost the whole bank. A voice with four
                        # of six vocalizations is usable; a voice with none is not, and
                        # raising here would have thrown away the ones that worked.
                        failed.append({"kind": kind, "variant": str(index), "error": str(exception)})
                        break

            elapsed = (time.perf_counter() - started) * 1000.0
            return {
                "succeeded": len(written) > 0,
                "message": (
                    f"Rendered {len(written)} vocalization clip(s) with {self.model_name} "
                    f"on {self.device}." +
                    (f" {len(failed)} failed." if failed else "")
                ),
                "written": written,
                "failed": failed,
                "elapsed_ms": elapsed,
                "device": self.device,
                "device_name": self.device_name,
                "dtype": self.dtype_name,
            }

    def prepare_voice(
        self,
        request: dict[str, Any],
        request_received: float | None = None,
    ) -> dict[str, Any]:
        received = request_received or time.perf_counter()
        lock_started = time.perf_counter()
        with self.lock:
            queue_wait_ms = (time.perf_counter() - lock_started) * 1000.0
            reference_audio = Path(
                self._text(request.get("reference_audio"), "Reference audio", 2048)
            ).expanduser().resolve()
            if not reference_audio.is_file() or reference_audio.suffix.lower() != ".wav":
                raise ValueError("The voice reference WAV does not exist.")
            reference_text = self._text(
                request.get("reference_text"), "Reference transcript", 1000)
            model_started = time.perf_counter()
            model = self._load("clone")
            model_ready_ms = (time.perf_counter() - model_started) * 1000.0
            prompt_started = time.perf_counter()
            _, cache_hit = self._voice_prompt(model, reference_audio, reference_text)
            prompt_ms = (time.perf_counter() - prompt_started) * 1000.0
            self.last_used = time.time()
            elapsed = (time.perf_counter() - received) * 1000.0
            return {
                "succeeded": True,
                "message": f"Active voice ready on {self.device}.",
                "elapsed_ms": elapsed,
                "worker_queue_wait_ms": queue_wait_ms,
                "model_ready_ms": model_ready_ms,
                "clone_prompt_ms": prompt_ms,
                "clone_prompt_cached": cache_hit,
                "device": self.device,
                "device_name": self.device_name,
                "dtype": self.dtype_name,
                "attention_backend": self.attention_backend,
                "cuda_math_mode": self.cuda_math_mode,
                "input_mode": self.args.input_mode,
                # Reported here because this is the first call that loads the clone
                # model, and therefore the first moment the runtime knows whether the
                # low-latency path installed. The caller logs it at startup, so a
                # session that fell back to stock generation says so before the first
                # phrase rather than being inferred from a slow one.
                **self._backend_state(),
            }

    def _synthesize(
        self,
        request: dict[str, Any],
        in_memory: bool,
        request_received: float | None = None,
    ) -> tuple[dict[str, Any], bytes | None]:
        received = request_received or time.perf_counter()
        lock_started = time.perf_counter()
        with self.lock:
            queue_wait_ms = (time.perf_counter() - lock_started) * 1000.0
            text = self._text(request.get("text"), "Speech text", 5000)
            language = self._text(request.get("language", "English"), "Language", 40)
            reference_audio = Path(
                self._text(request.get("reference_audio"), "Reference audio", 2048)
            ).expanduser().resolve()
            if not reference_audio.is_file() or reference_audio.suffix.lower() != ".wav":
                raise ValueError("The voice reference WAV does not exist.")
            reference_text = self._text(request.get("reference_text"), "Reference transcript", 1000)
            output = None if in_memory else self._validated_output(
                self._text(request.get("output_path"), "Output path", 2048))
            model_started = time.perf_counter()
            model = self._load("clone")
            model_ready_ms = (time.perf_counter() - model_started) * 1000.0
            import soundfile as sf

            prompt_started = time.perf_counter()
            voice_clone_prompt, cache_hit = self._voice_prompt(
                model, reference_audio, reference_text)
            prompt_ms = (time.perf_counter() - prompt_started) * 1000.0
            prompt_key = self._prompt_key(reference_audio, reference_text)
            audio_cache_key = (
                prompt_key, text, language, self.args.input_mode, self.attention_backend)
            cached_audio = self.reflex_audio_cache.get(audio_cache_key) if (
                in_memory and text in REFLEX_CACHE_TEXT) else None
            if cached_audio is not None:
                audio, sample_rate, audio_duration_ms = cached_audio
                self.last_used = time.time()
                elapsed = (time.perf_counter() - received) * 1000.0
                return {
                    "succeeded": True,
                    "message": "Speech served from the bounded Reflex voice cache.",
                    "output_path": "",
                    "elapsed_ms": elapsed,
                    "worker_queue_wait_ms": queue_wait_ms,
                    "model_ready_ms": model_ready_ms,
                    "clone_prompt_ms": prompt_ms,
                    "clone_prompt_cached": cache_hit,
                    "generation_ms": 0.0,
                    "first_audio_chunk_ms": 0.0,
                    "wav_write_ms": 0.0,
                    "sample_rate": sample_rate,
                    "audio_duration_ms": audio_duration_ms,
                    "device": self.device,
                    "device_name": self.device_name,
                    "dtype": self.dtype_name,
                    "attention_backend": self.attention_backend,
                    "cuda_math_mode": self.cuda_math_mode,
                    "input_mode": self.args.input_mode,
                    "audio_cache_hit": True,
                    "true_incremental_audio": False,
                    **self._backend_state(),
                    "model_resident": True,
                    "real_time_factor": 0.0,
                    **self._gpu_reading(),
                }, audio
            generation_started = time.perf_counter()
            waveforms, sample_rate = model.generate_voice_clone(
                text=text,
                language=language,
                voice_clone_prompt=voice_clone_prompt,
                # Every request already contains a complete natural phrase. The package
                # documents False as simulated streaming TEXT input, not incremental
                # audio output; complete mode avoids paying that simulation overhead.
                non_streaming_mode=self.args.input_mode == "complete",
            )
            generation_ms = (time.perf_counter() - generation_started) * 1000.0
            encoding_started = time.perf_counter()
            audio: bytes | None = None
            if in_memory:
                buffer = io.BytesIO()
                sf.write(buffer, waveforms[0], sample_rate, format="WAV")
                audio = buffer.getvalue()
                if len(audio) > self.args.max_audio_mib * 1024 * 1024:
                    raise ValueError(
                        f"Generated audio exceeds the {self.args.max_audio_mib} MiB buffer limit.")
            else:
                sf.write(output, waveforms[0], sample_rate)
            wav_write_ms = (time.perf_counter() - encoding_started) * 1000.0
            self.last_used = time.time()
            elapsed = (time.perf_counter() - received) * 1000.0
            audio_duration_ms = (
                float(len(waveforms[0])) * 1000.0 / float(sample_rate)
                if sample_rate else 0.0
            )
            if audio is not None and text in REFLEX_CACHE_TEXT:
                if len(self.reflex_audio_cache) >= len(REFLEX_CACHE_TEXT):
                    self.reflex_audio_cache.pop(next(iter(self.reflex_audio_cache)))
                self.reflex_audio_cache[audio_cache_key] = (
                    audio, sample_rate, audio_duration_ms)
            return {
                "succeeded": True,
                "message": f"Speech synthesized with {self.model_name} on {self.device}.",
                "output_path": str(output) if output is not None else "",
                "elapsed_ms": elapsed,
                "worker_queue_wait_ms": queue_wait_ms,
                "model_ready_ms": model_ready_ms,
                "clone_prompt_ms": prompt_ms,
                "clone_prompt_cached": cache_hit,
                "generation_ms": generation_ms,
                # True first audio is not exposed by this package. For this batch API,
                # the first playable chunk becomes available when generation finishes.
                "first_audio_chunk_ms": generation_ms,
                "wav_write_ms": wav_write_ms,
                "sample_rate": sample_rate,
                "audio_duration_ms": audio_duration_ms,
                "device": self.device,
                "device_name": self.device_name,
                "dtype": self.dtype_name,
                "attention_backend": self.attention_backend,
                "cuda_math_mode": self.cuda_math_mode,
                "input_mode": self.args.input_mode,
                "audio_cache_hit": False,
                "true_incremental_audio": False,
                # Whether this request found the model already on the device. A reload
                # costs seconds and is the first thing worth ruling out on a slow
                # utterance, so it is answered per request rather than inferred.
                **self._backend_state(),
                "model_resident": model_ready_ms < 1.0,
                # Generation time over the audio it produced. Above 1.0 the pipeline
                # cannot keep up with playback and every queued phrase falls further
                # behind, which is the difference between a slow voice and one that
                # never catches up.
                "real_time_factor": (
                    generation_ms / audio_duration_ms if audio_duration_ms > 0.0 else -1.0
                ),
                **self._gpu_reading(),
            }, audio

    # Peak allocation above the resident model, fitted to measured batches.
    #
    # Two terms, because one does not describe the data. Fitting only per character
    # underestimates a batch of several short phrases by more than a factor of two --
    # five phrases totalling 195 characters cost 1412 MiB, where 472 characters in four
    # longer phrases cost 1565 MiB. Each sequence carries a fixed cost of its own, so
    # the estimate carries one too.
    #
    # Fitted on an RTX 5070 in bfloat16 over batches of 4, 5, 8 and 12 phrases. Float32
    # holds twice the activation width for the same work, so both terms double there.
    # Used only to refuse a batch, never to size one: too pessimistic costs a fallback
    # to the per-phrase path, too optimistic costs the process.
    BATCH_MIB_PER_CHARACTER_BF16 = 1.5
    BATCH_MIB_PER_PHRASE_BF16 = 240.0

    def _batch_fits(self, characters: int, phrases: int) -> tuple[bool, str, int]:
        """Whether a batch of this shape can be attempted on the current device."""
        if not self.device.startswith("cuda:"):
            return True, "", -1
        try:
            import torch

            index = int(self.device.split(":", 1)[1])
            free_bytes, _ = torch.cuda.mem_get_info(index)
            # Blocks PyTorch has already taken from the driver and is no longer using.
            # They are free for this allocation without touching the driver at all, and
            # leaving them out is what made every batch after the first one refuse
            # itself: the first batch's arena stays reserved, the driver reports it as
            # taken, and a card with gigabytes of reusable cache looks full.
            reserved = torch.cuda.memory_reserved(index)
            allocated = torch.cuda.memory_allocated(index)
            reusable = max(0, reserved - allocated)
        except Exception:
            # An unreadable card is not an argument for attempting the larger thing.
            return False, "Free VRAM could not be read; using the per-phrase path.", -1
        available_mib = int((free_bytes + reusable) // (1024 * 1024))
        scale = 1.0 if self.dtype_name == "bfloat16" else 2.0
        projected = int(
            (characters * self.BATCH_MIB_PER_CHARACTER_BF16
             + phrases * self.BATCH_MIB_PER_PHRASE_BF16) * scale
        )
        # Reuse the reserve the operator already set for this device rather than
        # inventing a second, differently-shaped headroom rule.
        reserve = max(0, int(self.args.minimum_free_vram_mib) // 4)
        if projected + reserve > available_mib:
            return False, (
                "Projected batch peak " + str(projected) + " MiB plus " + str(reserve)
                + " MiB reserve exceeds " + str(available_mib) + " MiB available"
            ), projected
        return True, "", projected

    def synthesize_batch(
        self,
        request: dict[str, Any],
        request_received: float | None = None,
    ) -> tuple[dict[str, Any], list[bytes]]:
        """Synthesize several complete phrases in one generation call.

        Returns audio in the order the texts were given. The caller depends on that
        ordering to keep playback in reply order, so a result count that does not match
        the request is reported as a failure rather than repaired by guessing which
        phrase went missing.
        """
        received = request_received or time.perf_counter()
        lock_started = time.perf_counter()
        with self.lock:
            queue_wait_ms = (time.perf_counter() - lock_started) * 1000.0
            raw_texts = request.get("texts")
            if not isinstance(raw_texts, list) or not raw_texts:
                raise ValueError("Batch synthesis requires a non-empty texts array.")
            if len(raw_texts) > 32:
                raise ValueError("Batch synthesis accepts at most 32 phrases.")
            texts = [self._text(value, "Speech text", 5000) for value in raw_texts]
            language = self._text(request.get("language", "English"), "Language", 40)
            reference_audio = Path(
                self._text(request.get("reference_audio"), "Reference audio", 2048)
            ).expanduser().resolve()
            if not reference_audio.is_file() or reference_audio.suffix.lower() != ".wav":
                raise ValueError("The voice reference WAV does not exist.")
            reference_text = self._text(
                request.get("reference_text"), "Reference transcript", 1000)

            characters = sum(len(value) for value in texts)
            model_started = time.perf_counter()
            model = self._load("clone")
            model_ready_ms = (time.perf_counter() - model_started) * 1000.0

            fits, refusal, projected_mib = self._batch_fits(characters, len(texts))
            if not fits:
                # Refused, not failed. The caller has a working per-phrase path, so this
                # is a normal reply describing why that path should be used.
                return {
                    "succeeded": False,
                    "batch_refused": True,
                    "message": refusal,
                    "phrases": len(texts),
                    "characters": characters,
                    "projected_peak_mib": projected_mib,
                    **self._gpu_reading(),
                }, []

            import soundfile as sf

            prompt_started = time.perf_counter()
            voice_clone_prompt, cache_hit = self._voice_prompt(
                model, reference_audio, reference_text)
            prompt_ms = (time.perf_counter() - prompt_started) * 1000.0

            if self.device.startswith("cuda:"):
                try:
                    import torch

                    torch.cuda.reset_peak_memory_stats(int(self.device.split(":", 1)[1]))
                except Exception:
                    pass

            generation_started = time.perf_counter()
            waveforms, sample_rate = model.generate_voice_clone(
                text=texts,
                language=[language] * len(texts),
                voice_clone_prompt=voice_clone_prompt,
                non_streaming_mode=self.args.input_mode == "complete",
            )
            generation_ms = (time.perf_counter() - generation_started) * 1000.0

            if len(waveforms) != len(texts):
                raise RuntimeError(
                    "Batch returned " + str(len(waveforms)) + " clips for "
                    + str(len(texts)) + " phrases."
                )

            encoding_started = time.perf_counter()
            clips: list[bytes] = []
            durations: list[float] = []
            for waveform in waveforms:
                buffer = io.BytesIO()
                sf.write(buffer, waveform, sample_rate, format="WAV")
                clips.append(buffer.getvalue())
                durations.append(
                    float(len(waveform)) * 1000.0 / float(sample_rate)
                    if sample_rate else 0.0
                )
            wav_write_ms = (time.perf_counter() - encoding_started) * 1000.0

            total_bytes = sum(len(clip) for clip in clips)
            if total_bytes > self.args.max_audio_mib * 1024 * 1024:
                raise ValueError("Batched audio exceeds the configured buffer limit.")

            peak_mib = -1
            if self.device.startswith("cuda:"):
                try:
                    import torch

                    peak_mib = int(torch.cuda.max_memory_allocated(
                        int(self.device.split(":", 1)[1])) // (1024 * 1024))
                except Exception:
                    pass

            self.last_used = time.time()
            audio_duration_ms = sum(durations)
            return {
                "succeeded": True,
                "batch_refused": False,
                "message": str(len(texts)) + " phrases synthesized on " + self.device,
                "elapsed_ms": (time.perf_counter() - received) * 1000.0,
                "worker_queue_wait_ms": queue_wait_ms,
                "model_ready_ms": model_ready_ms,
                "clone_prompt_ms": prompt_ms,
                "clone_prompt_cached": cache_hit,
                "generation_ms": generation_ms,
                "wav_write_ms": wav_write_ms,
                "sample_rate": sample_rate,
                "phrases": len(texts),
                "characters": characters,
                "clip_bytes": [len(clip) for clip in clips],
                "clip_duration_ms": durations,
                "audio_duration_ms": audio_duration_ms,
                "peak_vram_mib": peak_mib,
                "projected_peak_mib": projected_mib,
                "device": self.device,
                "device_name": self.device_name,
                "dtype": self.dtype_name,
                "attention_backend": self.attention_backend,
                "input_mode": self.args.input_mode,
                **self._backend_state(),
                "model_resident": model_ready_ms < 1.0,
                "real_time_factor": (
                    generation_ms / audio_duration_ms if audio_duration_ms > 0.0 else -1.0
                ),
                **self._gpu_reading(),
            }, clips

    def synthesize(
        self,
        request: dict[str, Any],
        request_received: float | None = None,
    ) -> dict[str, Any]:
        result, _ = self._synthesize(request, False, request_received)
        return result

    def synthesize_pcm(
        self,
        request: dict[str, Any],
        request_received: float | None = None,
    ) -> tuple[dict[str, Any], bytes]:
        result, audio = self._synthesize(request, True, request_received)
        if audio is None:
            raise RuntimeError("The in-memory WAV encoder returned no audio.")
        return result, audio


def make_handler(runtime: QwenRuntime, token: str) -> type[BaseHTTPRequestHandler]:
    class Handler(BaseHTTPRequestHandler):
        server_version = "ReviaQwenTTS/1.0"

        def log_message(self, format_string: str, *args: Any) -> None:
            print(f"[Qwen3-TTS HTTP] {format_string % args}", flush=True)

        def _authorized(self) -> bool:
            return self.headers.get("Authorization", "") == f"Bearer {token}"

        def _send(self, status: int, payload: dict[str, Any]) -> None:
            body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def _send_audio(self, metadata: dict[str, Any], audio: bytes) -> None:
            self.send_response(200)
            self.send_header("Content-Type", "audio/wav")
            self.send_header("Content-Length", str(len(audio)))
            self.send_header("Cache-Control", "no-store")
            headers = {
                "X-Revia-Elapsed-Ms": metadata.get("elapsed_ms", -1),
                "X-Revia-Queue-Ms": metadata.get("worker_queue_wait_ms", -1),
                "X-Revia-Model-Ready-Ms": metadata.get("model_ready_ms", -1),
                "X-Revia-Prompt-Ms": metadata.get("clone_prompt_ms", -1),
                "X-Revia-Generation-Ms": metadata.get("generation_ms", -1),
                "X-Revia-Wav-Write-Ms": metadata.get("wav_write_ms", -1),
                "X-Revia-Audio-Duration-Ms": metadata.get("audio_duration_ms", -1),
                "X-Revia-Sample-Rate": metadata.get("sample_rate", 0),
                "X-Revia-Prompt-Cached": int(bool(metadata.get("clone_prompt_cached", False))),
                "X-Revia-Audio-Cache-Hit": int(bool(metadata.get("audio_cache_hit", False))),
                "X-Revia-Device": metadata.get("device", ""),
                "X-Revia-Device-Name": metadata.get("device_name", ""),
                "X-Revia-Dtype": metadata.get("dtype", ""),
                "X-Revia-Attention": metadata.get("attention_backend", "auto"),
                "X-Revia-Input-Mode": metadata.get("input_mode", "complete"),
                "X-Revia-Vram-Used-Mib": metadata.get("vram_used_mib", -1),
                "X-Revia-Vram-Total-Mib": metadata.get("vram_total_mib", -1),
                "X-Revia-Gpu-Utilization": metadata.get("gpu_utilization", -1),
                "X-Revia-Model-Resident": int(bool(metadata.get("model_resident", False))),
                "X-Revia-Backend": metadata.get("backend", "standard"),
                "X-Revia-Cuda-Graph": int(bool(metadata.get("cuda_graph", False))),
                "X-Revia-Talker-Graph": int(bool(metadata.get("talker_graph", False))),
                "X-Revia-Real-Time-Factor": metadata.get("real_time_factor", -1),
            }
            for name, value in headers.items():
                self.send_header(name, str(value).replace("\r", " ").replace("\n", " "))
            self.end_headers()
            self.wfile.write(audio)

        def _send_audio_batch(
            self, metadata: dict[str, Any], clips: list[bytes]
        ) -> None:
            payload = b"".join(clips)
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(payload)))
            self.send_header("Cache-Control", "no-store")
            headers = {
                "X-Revia-Batch-Count": len(clips),
                # Byte length of each clip, in order. Splitting on these keeps the reply
                # order the caller depends on for ordered playback.
                "X-Revia-Batch-Sizes": ",".join(str(len(clip)) for clip in clips),
                "X-Revia-Batch-Durations-Ms": ",".join(
                    str(int(value)) for value in metadata.get("clip_duration_ms", [])
                ),
                "X-Revia-Batch-Characters": metadata.get("characters", 0),
                "X-Revia-Elapsed-Ms": metadata.get("elapsed_ms", -1),
                "X-Revia-Queue-Ms": metadata.get("worker_queue_wait_ms", -1),
                "X-Revia-Model-Ready-Ms": metadata.get("model_ready_ms", -1),
                "X-Revia-Prompt-Ms": metadata.get("clone_prompt_ms", -1),
                "X-Revia-Generation-Ms": metadata.get("generation_ms", -1),
                "X-Revia-Wav-Write-Ms": metadata.get("wav_write_ms", -1),
                "X-Revia-Audio-Duration-Ms": metadata.get("audio_duration_ms", -1),
                "X-Revia-Sample-Rate": metadata.get("sample_rate", 0),
                "X-Revia-Prompt-Cached": int(bool(metadata.get("clone_prompt_cached", False))),
                "X-Revia-Device": metadata.get("device", ""),
                "X-Revia-Device-Name": metadata.get("device_name", ""),
                "X-Revia-Dtype": metadata.get("dtype", ""),
                "X-Revia-Attention": metadata.get("attention_backend", "auto"),
                "X-Revia-Input-Mode": metadata.get("input_mode", "complete"),
                "X-Revia-Vram-Used-Mib": metadata.get("vram_used_mib", -1),
                "X-Revia-Vram-Total-Mib": metadata.get("vram_total_mib", -1),
                "X-Revia-Peak-Vram-Mib": metadata.get("peak_vram_mib", -1),
                "X-Revia-Gpu-Utilization": metadata.get("gpu_utilization", -1),
                "X-Revia-Model-Resident": int(bool(metadata.get("model_resident", False))),
                "X-Revia-Backend": metadata.get("backend", "standard"),
                "X-Revia-Cuda-Graph": int(bool(metadata.get("cuda_graph", False))),
                "X-Revia-Talker-Graph": int(bool(metadata.get("talker_graph", False))),
                "X-Revia-Real-Time-Factor": metadata.get("real_time_factor", -1),
            }
            for name, value in headers.items():
                self.send_header(name, str(value).replace("\r", " ").replace("\n", " "))
            self.end_headers()
            self.wfile.write(payload)

        def do_GET(self) -> None:  # noqa: N802 - required by BaseHTTPRequestHandler
            if not self._authorized():
                self._send(401, {"succeeded": False, "message": "Unauthorized."})
                return
            if self.path != "/health":
                self._send(404, {"succeeded": False, "message": "Not found."})
                return
            self._send(200, {
                "succeeded": True,
                "detail": "Qwen3-TTS worker is ready; models load on demand.",
                "model_kind": runtime.model_kind,
                "device": runtime.device,
                "device_name": runtime.device_name,
                "dtype": runtime.dtype_name,
                "attention_backend": runtime.attention_backend,
                "cuda_math_mode": runtime.cuda_math_mode,
                "input_mode": runtime.args.input_mode,
                **runtime._backend_state(),
                "loaded": runtime.model is not None,
                "loaded_at": runtime.loaded_at,
                "last_used": runtime.last_used,
                "clone_prompts": len(runtime.clone_prompts),
                "reflex_audio_cache_entries": len(runtime.reflex_audio_cache),
            })

        def do_POST(self) -> None:  # noqa: N802 - required by BaseHTTPRequestHandler
            if not self._authorized():
                self._send(401, {"succeeded": False, "message": "Unauthorized."})
                return
            try:
                request_received = time.perf_counter()
                length = int(self.headers.get("Content-Length", "0"))
                if length <= 0 or length > 64 * 1024:
                    raise ValueError("The request body is empty or too large.")
                request = json.loads(self.rfile.read(length).decode("utf-8"))
                if not isinstance(request, dict):
                    raise ValueError("The request must be a JSON object.")
                if self.path == "/v1/voice-design":
                    result = runtime.design(request)
                elif self.path == "/v1/audio/speech":
                    result = runtime.synthesize(request, request_received)
                elif self.path == "/v1/audio/pcm":
                    result, audio = runtime.synthesize_pcm(request, request_received)
                    self._send_audio(result, audio)
                    return
                elif self.path == "/v1/audio/pcm-batch":
                    result, clips = runtime.synthesize_batch(request, request_received)
                    if not result.get("succeeded", False):
                        # A refusal is a normal answer here: the caller falls back to the
                        # per-phrase path rather than treating it as a worker failure.
                        self._send(200, result)
                        return
                    self._send_audio_batch(result, clips)
                    return
                elif self.path == "/release":
                    with runtime.lock:
                        runtime._unload()
                    result = {"succeeded": True, "message": "Qwen3-TTS model memory released."}
                elif self.path == "/v1/vocalizations":
                    result = runtime.vocalizations(request)
                elif self.path == "/prepare-voice":
                    result = runtime.prepare_voice(request, request_received)
                elif self.path == "/prepare":
                    model_kind = request.get("model", "clone")
                    if model_kind not in ("clone", "design"):
                        raise ValueError("Prepare model must be 'clone' or 'design'.")
                    with runtime.lock:
                        started = time.perf_counter()
                        runtime._load(model_kind)
                        result = {
                            "succeeded": True,
                            "message": f"Qwen3-TTS {model_kind} model loaded on {runtime.device}.",
                            "elapsed_ms": (time.perf_counter() - started) * 1000.0,
                            "device": runtime.device,
                            "device_name": runtime.device_name,
                            "dtype": runtime.dtype_name,
                        }
                else:
                    self._send(404, {"succeeded": False, "message": "Not found."})
                    return
                self._send(200, result)
            except Exception as exception:
                # The message alone loses the frame that actually failed, which is the
                # one thing worth having. Print the full traceback.
                print(f"[Qwen3-TTS] Request failed: {exception}", file=sys.stderr, flush=True)
                traceback.print_exc(file=sys.stderr)
                sys.stderr.flush()

                if _is_context_fatal(exception):
                    # A device-side assert poisons the CUDA context for the lifetime of
                    # the process: every later call on that device raises the same error,
                    # so the worker would keep answering 500 forever and only the FIRST
                    # failure would be diagnostic. Answer this request honestly, then
                    # exit - Revia's EnsureAvailable restarts the worker on next use,
                    # and a fresh process gets a clean context.
                    print(
                        "[Qwen3-TTS] The CUDA context is unrecoverable; exiting so the "
                        "next request starts a clean worker.",
                        file=sys.stderr, flush=True)
                    self._send(500, {
                        "succeeded": False,
                        "message": (
                            f"{exception} -- the CUDA context is unrecoverable, so the "
                            "voice worker restarted. Retry the request."),
                    })
                    sys.stderr.flush()
                    sys.stdout.flush()
                    os._exit(70)

                self._send(500, {"succeeded": False, "message": str(exception)})

    return Handler


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Revia loopback Qwen3-TTS worker")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8092)
    parser.add_argument("--token", required=True)
    parser.add_argument("--device", default="auto")
    parser.add_argument("--minimum-free-vram-mib", type=int, default=4600)
    parser.add_argument("--cpu-threads", type=int, default=2)
    parser.add_argument("--max-audio-mib", type=int, default=128)
    parser.add_argument("--design-model", default="Qwen/Qwen3-TTS-12Hz-1.7B-VoiceDesign")
    parser.add_argument("--clone-model", default="Qwen/Qwen3-TTS-12Hz-0.6B-Base")
    parser.add_argument(
        "--attention-backend",
        choices=("adaptive", "auto", "eager", "sdpa", "flash_attention_2"),
        default="adaptive")
    # The low-latency path replaces the codebook predictor's inner generate loop with a
    # direct forward loop, and optionally replays that loop as a CUDA graph. Both are off
    # by default: the stock path is the one that has always worked, and a worker that
    # cannot capture a graph must still be able to speak.
    parser.add_argument("--low-latency", action="store_true")
    parser.add_argument("--cuda-graph", action="store_true")
    parser.add_argument("--talker-graph", action="store_true")
    parser.add_argument(
        "--input-mode", choices=("complete", "simulated-stream"), default="simulated-stream")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.host not in ("127.0.0.1", "localhost", "::1"):
        print("The Revia Qwen3-TTS worker only permits a loopback bind.", file=sys.stderr)
        return 2
    if not (1 <= args.port <= 65535):
        print("Invalid port.", file=sys.stderr)
        return 2
    if not (1 <= args.cpu_threads <= 64):
        print("Invalid CPU thread cap.", file=sys.stderr)
        return 2
    if not (16 <= args.max_audio_mib <= 2048):
        print("Invalid audio buffer limit.", file=sys.stderr)
        return 2
    runtime = QwenRuntime(args)
    server = ThreadingHTTPServer((args.host, args.port), make_handler(runtime, args.token))
    print(f"[Qwen3-TTS] Ready on http://{args.host}:{args.port}", flush=True)
    try:
        server.serve_forever(poll_interval=0.25)
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
