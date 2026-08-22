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
import json
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any


class ImageRuntime:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.lock = threading.Lock()
        self.pipeline: Any | None = None
        self.device = "cpu"
        self.device_name = "CPU"
        self.dtype_name = "float32"
        self.model_id = args.model

    def describe(self) -> dict[str, Any]:
        return {
            "loaded": self.pipeline is not None,
            "model": self.model_id,
            "device": self.device,
            "deviceName": self.device_name,
            "dtype": self.dtype_name,
        }

    def _select_device(self, torch: Any) -> tuple[str, Any, str]:
        """CUDA only when there is genuinely room, otherwise CPU.

        Loading onto a card that is nearly full does not fail cleanly; it either
        thrashes or dies partway through a step. Measuring first and choosing CPU
        is slower and finishes.
        """
        if self.args.device == "cpu" or not torch.cuda.is_available():
            return "cpu", torch.float32, "CPU"

        wanted = self.args.min_free_vram_mib * 1024 * 1024
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

        return "cpu", torch.float32, "CPU"

    def ensure_loaded(self) -> dict[str, Any]:
        with self.lock:
            if self.pipeline is not None:
                return self.describe()

            import torch
            from diffusers import AutoPipelineForText2Image

            device, dtype, device_name = self._select_device(torch)
            pipeline = AutoPipelineForText2Image.from_pretrained(
                self.model_id,
                torch_dtype=dtype,
                cache_dir=self.args.cache_dir or None,
                local_files_only=self.args.offline,
                safety_checker=None,
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
            return self.describe()

    def generate(self, payload: dict[str, Any]) -> dict[str, Any]:
        prompt = str(payload.get("prompt", "")).strip()
        if not prompt:
            raise ValueError("A prompt is required.")

        state = self.ensure_loaded()
        steps = max(1, min(int(payload.get("steps", self.args.steps)), 50))
        width = max(256, min(int(payload.get("width", self.args.width)), 1024))
        height = max(256, min(int(payload.get("height", self.args.height)), 1024))
        # Diffusers requires multiples of 8; rounding here beats an opaque failure.
        width -= width % 8
        height -= height % 8

        import torch

        generator = None
        seed = payload.get("seed")
        if seed is not None:
            generator = torch.Generator(device="cpu").manual_seed(int(seed))

        started = time.time()
        with self.lock:
            result = self.pipeline(
                prompt=prompt,
                negative_prompt=payload.get("negativePrompt") or None,
                num_inference_steps=steps,
                guidance_scale=float(payload.get("guidance", self.args.guidance)),
                width=width,
                height=height,
                generator=generator,
            )
        elapsed = (time.time() - started) * 1000.0

        output_path = Path(payload["outputPath"])
        output_path.parent.mkdir(parents=True, exist_ok=True)
        result.images[0].save(output_path, format="PNG")

        return {
            "ok": True,
            "path": str(output_path),
            "elapsedMs": round(elapsed, 1),
            "steps": steps,
            "width": width,
            "height": height,
            **state,
        }

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


def build_handler(runtime: ImageRuntime, api_key: str) -> type[BaseHTTPRequestHandler]:
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
            return self.headers.get("Authorization", "") == f"Bearer {api_key}"

        def do_GET(self) -> None:  # noqa: N802 - http.server naming
            if not self._authorised():
                self._reply(401, {"ok": False, "error": "unauthorised"})
                return
            if self.path == "/health":
                self._reply(200, {"ok": True, **runtime.describe()})
                return
            self._reply(404, {"ok": False, "error": "unknown endpoint"})

        def do_POST(self) -> None:  # noqa: N802 - http.server naming
            if not self._authorised():
                self._reply(401, {"ok": False, "error": "unauthorised"})
                return
            length = int(self.headers.get("Content-Length", "0"))
            try:
                payload = json.loads(self.rfile.read(length) or b"{}")
            except json.JSONDecodeError as error:
                self._reply(400, {"ok": False, "error": f"bad JSON: {error}"})
                return

            try:
                if self.path == "/generate":
                    self._reply(200, runtime.generate(payload))
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
    parser.add_argument("--device", default="auto")
    parser.add_argument("--min-free-vram-mib", type=int, default=4200)
    parser.add_argument("--steps", type=int, default=4)
    parser.add_argument("--guidance", type=float, default=1.0)
    parser.add_argument("--width", type=int, default=512)
    parser.add_argument("--height", type=int, default=512)
    parser.add_argument("--cache-dir", default="")
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
