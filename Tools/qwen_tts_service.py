"""Loopback-only Qwen3-TTS worker used by the C++ Revia runtime.

Models load lazily and only one model is resident at a time. This keeps voice
design and voice cloning functional on machines that cannot hold both models.
"""

from __future__ import annotations

import argparse
import gc
import json
import os
import sys
import threading
import traceback
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any




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
        self.clone_prompts: dict[tuple[str, str], Any] = {}

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
        print(f"[Qwen3-TTS] Loading {kind} model {model_name} on {self.device}: {device_reason}", flush=True)
        try:
            self.model = Qwen3TTSModel.from_pretrained(
                model_name,
                device_map=self.device,
                dtype=dtype,
            )
        except Exception:
            if self.device == "cpu":
                raise
            print("[Qwen3-TTS] CUDA model load failed; retrying on CPU.", flush=True)
            self._unload()
            self.device = "cpu"
            self.device_name = "CPU"
            self.dtype_name = "float32"
            self.model = Qwen3TTSModel.from_pretrained(
                model_name,
                device_map="cpu",
                dtype=torch.float32,
            )
        self.model_kind = kind
        self.model_name = model_name
        return self.model

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

    def synthesize(self, request: dict[str, Any]) -> dict[str, Any]:
        with self.lock:
            started = time.perf_counter()
            text = self._text(request.get("text"), "Speech text", 5000)
            language = self._text(request.get("language", "English"), "Language", 40)
            reference_audio = Path(
                self._text(request.get("reference_audio"), "Reference audio", 2048)
            ).expanduser().resolve()
            if not reference_audio.is_file() or reference_audio.suffix.lower() != ".wav":
                raise ValueError("The voice reference WAV does not exist.")
            reference_text = self._text(request.get("reference_text"), "Reference transcript", 1000)
            output = self._validated_output(self._text(request.get("output_path"), "Output path", 2048))
            model = self._load("clone")
            import soundfile as sf

            prompt_key = (str(reference_audio), reference_text)
            voice_clone_prompt = self.clone_prompts.get(prompt_key)
            if voice_clone_prompt is None:
                voice_clone_prompt = model.create_voice_clone_prompt(
                    ref_audio=str(reference_audio),
                    ref_text=reference_text,
                    x_vector_only_mode=False,
                )
                self.clone_prompts[prompt_key] = voice_clone_prompt
            waveforms, sample_rate = model.generate_voice_clone(
                text=text,
                language=language,
                voice_clone_prompt=voice_clone_prompt,
            )
            sf.write(output, waveforms[0], sample_rate)
            elapsed = (time.perf_counter() - started) * 1000.0
            return {
                "succeeded": True,
                "message": f"Speech synthesized with {self.model_name} on {self.device}.",
                "output_path": str(output),
                "elapsed_ms": elapsed,
                "device": self.device,
                "device_name": self.device_name,
                "dtype": self.dtype_name,
            }


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
            })

        def do_POST(self) -> None:  # noqa: N802 - required by BaseHTTPRequestHandler
            if not self._authorized():
                self._send(401, {"succeeded": False, "message": "Unauthorized."})
                return
            try:
                length = int(self.headers.get("Content-Length", "0"))
                if length <= 0 or length > 64 * 1024:
                    raise ValueError("The request body is empty or too large.")
                request = json.loads(self.rfile.read(length).decode("utf-8"))
                if not isinstance(request, dict):
                    raise ValueError("The request must be a JSON object.")
                if self.path == "/v1/voice-design":
                    result = runtime.design(request)
                elif self.path == "/v1/audio/speech":
                    result = runtime.synthesize(request)
                elif self.path == "/release":
                    with runtime.lock:
                        runtime._unload()
                    result = {"succeeded": True, "message": "Qwen3-TTS model memory released."}
                elif self.path == "/v1/vocalizations":
                    result = runtime.vocalizations(payload)
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
    parser.add_argument("--design-model", default="Qwen/Qwen3-TTS-12Hz-1.7B-VoiceDesign")
    parser.add_argument("--clone-model", default="Qwen/Qwen3-TTS-12Hz-0.6B-Base")
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
