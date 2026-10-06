"""Loopback-only image-generation worker used by the C++ Revia runtime.

Same shape as the Qwen3-TTS worker: the C++ side owns the process lifecycle, this
side owns only the model. The model loads lazily on the first request so startup
costs nothing on a machine that never asks for a picture, and the device is chosen
against measured free VRAM rather than assumed, because on a single-GPU laptop the
chat model has usually already taken it.
"""

from __future__ import annotations

import argparse
import gc
import hashlib
import hmac
import json
import math
import os
import re
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any


class GenerationCancelled(RuntimeError):
    pass


class ImageRuntime:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.lock = threading.RLock()
        self.pipeline: Any | None = None
        self.device = "cpu"
        self.device_name = "CPU"
        self.dtype_name = "float32"
        self.model_id = args.model
        self.jobs_lock = threading.RLock()
        self.jobs: dict[str, dict[str, Any]] = {}
        self.cancellations: dict[str, threading.Event] = {}
        self.active_job: str | None = None
        self.load_elapsed_ms = 0.0

    def describe(self) -> dict[str, Any]:
        return {
            "loaded": self.pipeline is not None,
            "model": self.model_id,
            "device": self.device,
            "deviceName": self.device_name,
            "dtype": self.dtype_name,
            "loadElapsedMs": self.load_elapsed_ms,
        }

    def _select_device(self, torch: Any) -> tuple[str, Any, str]:
        """CUDA only when there is genuinely room, otherwise CPU.

        Loading onto a card that is nearly full does not fail cleanly; it either
        thrashes or dies partway through a step. Measuring first and choosing CPU
        is slower and finishes.
        """
        if self.args.device == "cpu":
            return "cpu", torch.float32, "CPU"
        if not torch.cuda.is_available():
            if self.args.device != "auto":
                raise RuntimeError("The requested CUDA device is unavailable.")
            return "cpu", torch.float32, "CPU"
        minimum = self.args.min_free_vram_mib
        if self.model_id.lower().rstrip("/") == "stabilityai/sdxl-turbo":
            # The 512px/fp16 acceptance run reserved 9034 MiB; retain a working margin.
            minimum = max(minimum, 9500)
        wanted = (minimum + getattr(self.args, "gpu_reserve_mib", 0)) * 1024 * 1024
        candidates = range(torch.cuda.device_count())
        if self.args.device.startswith("cuda:"):
            candidates = [int(self.args.device.split(":", 1)[1])]

        for index in candidates:
            try:
                properties = torch.cuda.get_device_properties(index)
                with torch.cuda.device(index):
                    free_bytes, _ = torch.cuda.mem_get_info()
            except Exception:
                continue
            if free_bytes < wanted:
                continue
            # Turing supports FP16 but not BF16; asking for the wrong one turns a
            # perfectly usable secondary card into a runtime failure.
            dtype = torch.float16
            return f"cuda:{index}", dtype, properties.name

        if self.args.device != "auto":
            raise RuntimeError("The requested CUDA device lacks the image budget and shared GPU reserve.")
        return "cpu", torch.float32, "CPU"

    def ensure_loaded(self) -> dict[str, Any]:
        with self.lock:
            if self.pipeline is not None:
                return self.describe()

            import torch
            from diffusers import AutoPipelineForText2Image

            started = time.monotonic()
            torch.set_num_threads(max(1, getattr(self.args, "cpu_threads", 4)))
            device, dtype, device_name = self._select_device(torch)
            if device.startswith("cuda:"):
                torch.cuda.reset_peak_memory_stats(device)
            pipeline = AutoPipelineForText2Image.from_pretrained(
                self.model_id,
                torch_dtype=dtype,
                cache_dir=self.args.cache_dir or None,
                local_files_only=self.args.offline,
                variant=getattr(self.args, "variant", "") or None,
            )
            pipeline = pipeline.to(device)
            pipeline.set_progress_bar_config(disable=True)
            if device == "cpu":
                # Attention slicing trades a little speed for a much smaller peak,
                # which is what makes CPU generation finish rather than swap.
                pipeline.enable_attention_slicing()

            self.pipeline = pipeline
            self.device = device
            self.device_name = device_name
            self.dtype_name = str(dtype).replace("torch.", "")
            self.load_elapsed_ms = (time.monotonic() - started) * 1000.0
            return self.describe()

    def validate_destination(self, payload: dict[str, Any]) -> Path:
        prompt = str(payload.get("prompt", "")).strip()
        if not prompt:
            raise ValueError("A prompt is required.")
        # Checked before the GPU is spent: a missing destination used to surface as a
        # bare KeyError only after the whole image had been generated.
        raw_output = payload.get("outputPath")
        if not isinstance(raw_output, str) or not raw_output.strip():
            raise ValueError("An outputPath is required.")
        output_path = Path(raw_output).resolve()
        root = getattr(self.args, "output_root", "")
        if root and not output_path.is_relative_to(Path(root).resolve()):
            raise ValueError("The output path is outside the owned output directory.")
        if output_path.suffix.lower() != ".png":
            raise ValueError("The output must be a PNG file.")
        if output_path.exists():
            raise ValueError("The output file already exists.")
        return output_path

    def generation_options(self, payload: dict[str, Any]) -> dict[str, Any]:
        try:
            steps = max(1, min(int(payload.get("steps", self.args.steps)), 50))
            width = max(256, min(int(payload.get("width", self.args.width)), 1024))
            height = max(256, min(int(payload.get("height", self.args.height)), 1024))
            guidance = float(payload.get("guidance", self.args.guidance))
            if payload.get("seed") is not None:
                int(payload["seed"])
        except (TypeError, ValueError, OverflowError) as error:
            raise ValueError("Generation options must be valid numbers.") from error
        if not math.isfinite(guidance):
            raise ValueError("Guidance must be finite.")
        # Diffusers requires multiples of 8; rounding here beats an opaque failure.
        width -= width % 8
        height -= height % 8
        turbo = self.model_id.lower().rstrip("/") in ("stabilityai/sd-turbo", "stabilityai/sdxl-turbo")
        return {"num_inference_steps": min(steps, 4) if turbo else steps,
                "guidance_scale": 0.0 if turbo else guidance,
                "negative_prompt": None if turbo else payload.get("negativePrompt") or None,
                "width": width, "height": height}

    def generate(self, payload: dict[str, Any], cancellation: threading.Event | None = None) -> dict[str, Any]:
        output_path = self.validate_destination(payload)
        options = self.generation_options(payload)
        cancellation = cancellation or threading.Event()
        job_id = str(payload.get("jobId", ""))

        def check_cancel() -> None:
            if cancellation.is_set():
                raise GenerationCancelled("Image generation was cancelled.")

        def step_end(_pipeline, step, _timestep, callback_kwargs):
            check_cancel()
            with self.jobs_lock:
                if job_id in self.jobs:
                    self.jobs[job_id].update(state="generating", step=step + 1)
            return callback_kwargs

        with self.lock:
            # Loading and inference share ownership so unload cannot discard the
            # pipeline after ensure_loaded reports it ready.
            check_cancel()
            state = self.ensure_loaded()
            check_cancel()
            import torch

            generator = None
            seed = payload.get("seed")
            if seed is not None:
                generator = torch.Generator(device="cpu").manual_seed(int(seed))

            with self.jobs_lock:
                if job_id in self.jobs:
                    self.jobs[job_id].update(state="generating", **state)
            started = time.monotonic()
            result = self.pipeline(
                prompt=str(payload["prompt"]).strip(),
                generator=generator,
                callback_on_step_end=step_end,
                **options,
            )
            check_cancel()
            elapsed = (time.monotonic() - started) * 1000.0
            peak_allocated = torch.cuda.max_memory_allocated(self.device) / (1024 * 1024) if self.device.startswith("cuda:") else 0.0
            peak_reserved = torch.cuda.max_memory_reserved(self.device) / (1024 * 1024) if self.device.startswith("cuda:") else 0.0
            output_path.parent.mkdir(parents=True, exist_ok=True)
            temporary = output_path.with_suffix(".pending.png")
            try:
                result.images[0].save(temporary, format="PNG")
                from PIL import Image

                with Image.open(temporary) as decoded:
                    decoded.load()
                    if decoded.format != "PNG" or decoded.size != (options["width"], options["height"]):
                        raise ValueError("The generated image dimensions or format do not match the request.")
                digest = hashlib.sha256(temporary.read_bytes()).hexdigest()
                with self.jobs_lock:
                    check_cancel()
                    self.validate_destination(payload)
                    # A hard link publishes atomically without replacing an existing artifact.
                    os.link(temporary, output_path)
            finally:
                temporary.unlink(missing_ok=True)
        return {
            "ok": True,
            "path": str(output_path),
            "elapsedMs": round(elapsed, 1),
            "peakAllocatedMiB": round(peak_allocated, 1),
            "peakReservedMiB": round(peak_reserved, 1),
            "jobId": job_id,
            "sha256": digest,
            "seed": seed,
            "guidance": options["guidance_scale"],
            "steps": options["num_inference_steps"],
            "width": options["width"],
            "height": options["height"],
            **state,
        }

    def submit(self, payload: dict[str, Any]) -> dict[str, Any]:
        self.validate_destination(payload)
        options = self.generation_options(payload)
        job_id = payload.get("jobId", "")
        if not isinstance(job_id, str) or not re.fullmatch(r"[a-zA-Z0-9_-]{1,96}", job_id):
            raise ValueError("A bounded unique jobId is required.")
        with self.jobs_lock:
            if self.active_job is not None:
                raise RuntimeError("The image worker is busy with another job.")
            if job_id in self.jobs:
                raise ValueError("This jobId already exists; inspect its status before retrying.")
            while len(self.jobs) >= 16:
                old = next(iter(self.jobs))
                del self.jobs[old]
                del self.cancellations[old]
            self.active_job = job_id
            self.cancellations[job_id] = threading.Event()
            self.jobs[job_id] = {"ok": True, "jobId": job_id, "state": "loading", "step": 0,
                                 "steps": options["num_inference_steps"], "model": self.model_id}
            threading.Thread(target=self._run_job, args=(dict(payload),), daemon=True).start()
            return dict(self.jobs[job_id])

    def _run_job(self, payload: dict[str, Any]) -> None:
        job_id = payload["jobId"]
        cancellation = self.cancellations[job_id]
        result = {}
        try:
            result = self.generate(payload, cancellation)
            outcome = dict(result, state="succeeded")
        except GenerationCancelled as error:
            outcome = {"ok": False, "state": "cancelled", "error": str(error)}
        except Exception as error:
            outcome = {"ok": False, "state": "failed", "error": str(error)}
        finally:
            if not getattr(self.args, "keep_loaded", False):
                self.unload()
            with self.jobs_lock:
                if cancellation.is_set():
                    if "path" in result:
                        Path(result["path"]).unlink(missing_ok=True)
                    outcome = {"ok": False, "state": "cancelled", "error": "Image generation was cancelled."}
                self.jobs[job_id].update(outcome, loaded=self.pipeline is not None)
                self.active_job = None

    def job_status(self, job_id: str) -> dict[str, Any]:
        with self.jobs_lock:
            if job_id not in self.jobs:
                raise KeyError("Unknown image job.")
            return dict(self.jobs[job_id])

    def cancel(self, job_id: str) -> dict[str, Any]:
        with self.jobs_lock:
            if job_id not in self.jobs:
                raise KeyError("Unknown image job.")
            if self.jobs[job_id]["state"] in ("loading", "generating"):
                self.cancellations[job_id].set()
                self.jobs[job_id]["state"] = "cancelling"
            return dict(self.jobs[job_id])

    def unload(self) -> dict[str, Any]:
        with self.lock:
            self.pipeline = None
            gc.collect()
            try:
                import torch

                if torch.cuda.is_available():
                    torch.cuda.empty_cache()
            except Exception:
                pass
            self.device = "cpu"
            self.device_name = "CPU"
            return self.describe()


# Far above any real request (a prompt and a few numbers), far below a memory problem.
MAX_REQUEST_BYTES = 64 * 1024


def build_handler(runtime: ImageRuntime, api_key: str) -> type[BaseHTTPRequestHandler]:
    expected_authorization = f"Bearer {api_key}".encode("utf-8")

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *_: Any) -> None:  # noqa: D401 - quiet by design
            """Access logging goes to Revia's own logs, not to stderr per request."""

        def _reply(self, code: int, body: dict[str, Any]) -> None:
            encoded = json.dumps(body).encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(encoded)))
            self.end_headers()
            self.wfile.write(encoded)

        def _authorised(self) -> bool:
            if not api_key:
                return True
            # Constant-time, and over bytes: compare_digest raises on a str holding
            # anything beyond ASCII, and a header holds whatever the client sent.
            supplied = self.headers.get("Authorization", "").encode("utf-8", "surrogateescape")
            return hmac.compare_digest(supplied, expected_authorization)

        def do_GET(self) -> None:  # noqa: N802 - http.server naming
            if not self._authorised():
                self._reply(401, {"ok": False, "error": "unauthorised"})
                return
            if self.path == "/health":
                self._reply(200, {"ok": True, **runtime.describe()})
                return
            if self.path.startswith("/jobs/"):
                try:
                    self._reply(200, runtime.job_status(self.path.removeprefix("/jobs/")))
                except KeyError as error:
                    self._reply(404, {"ok": False, "error": str(error)})
                return
            self._reply(404, {"ok": False, "error": "unknown endpoint"})

        def do_POST(self) -> None:  # noqa: N802 - http.server naming
            if not self._authorised():
                # Refused unread, so the connection cannot be reused for another request.
                self.close_connection = True
                self._reply(401, {"ok": False, "error": "unauthorised"})
                return
            # Parsed and bounded before anything is read: a malformed length used to
            # raise outside every handler, and an unbounded one was read in full.
            try:
                length = int(self.headers.get("Content-Length", "0"))
            except ValueError:
                length = -1
            if length < 0 or length > MAX_REQUEST_BYTES:
                # The body stays unread, so this connection cannot carry another request.
                self.close_connection = True
                self._reply(413 if length > MAX_REQUEST_BYTES else 400,
                            {"ok": False, "error": "request body is missing or too large"})
                return
            try:
                payload = json.loads(self.rfile.read(length) or b"{}")
            except (json.JSONDecodeError, UnicodeDecodeError) as error:
                self._reply(400, {"ok": False, "error": f"bad JSON: {error}"})
                return
            if not isinstance(payload, dict):
                self._reply(400, {"ok": False, "error": "the request must be a JSON object"})
                return

            try:
                if self.path == "/generate":
                    self._reply(200, runtime.generate(payload))
                elif self.path == "/jobs":
                    self._reply(202, runtime.submit(payload))
                elif self.path.startswith("/jobs/") and self.path.endswith("/cancel"):
                    self._reply(200, runtime.cancel(self.path[len("/jobs/"):-len("/cancel")]))
                elif self.path == "/unload":
                    self._reply(200, {"ok": True, **runtime.unload()})
                else:
                    self._reply(404, {"ok": False, "error": "unknown endpoint"})
            except Exception as error:  # surfaced to the C++ side as a clean message
                self._reply(500, {"ok": False, "error": str(error)})

    return Handler


def main() -> int:
    parser = argparse.ArgumentParser(description="Revia local image worker")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8093)
    parser.add_argument("--model", default="stabilityai/sd-turbo")
    parser.add_argument("--variant", default="")
    parser.add_argument("--device", default="auto")
    parser.add_argument("--min-free-vram-mib", type=int, default=4200)
    parser.add_argument("--steps", type=int, default=4)
    parser.add_argument("--guidance", type=float, default=0.0)
    parser.add_argument("--width", type=int, default=512)
    parser.add_argument("--height", type=int, default=512)
    parser.add_argument("--cache-dir", default="")
    parser.add_argument("--output-root", default="RuntimeData/Images")
    parser.add_argument("--gpu-reserve-mib", type=int, default=1536)
    parser.add_argument("--cpu-threads", type=int, default=4)
    parser.add_argument("--keep-loaded", action="store_true")
    parser.add_argument("--offline", action="store_true")
    parser.add_argument("--api-key", default="")
    args = parser.parse_args()

    if args.host not in ("127.0.0.1", "localhost", "::1"):
        # The same rule the rest of the runtime follows: this worker is a local
        # implementation detail, never a network service.
        print("refusing to bind anything but loopback", file=sys.stderr, flush=True)
        return 2

    runtime = ImageRuntime(args)
    server = ThreadingHTTPServer((args.host, args.port), build_handler(runtime, args.api_key))
    print(f"revia-image ready on {args.host}:{args.port}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
