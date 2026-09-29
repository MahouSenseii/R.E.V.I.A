#!/usr/bin/env python3
"""Kokoro-82M as a fallback voice worker, on the same HTTP contract as the Qwen3-TTS worker.

Revia's speech service talks to one contract: /health, /prepare, /v1/audio/speech,
/v1/audio/pcm and /v1/audio/pcm-stream, bearer-authenticated, with the timing headers
the client reads. This worker answers that contract with Kokoro (Apache-2.0, 82M
parameters, CPU-capable), so that when every Qwen worker is down or its card is full
she still has a voice that is not Windows SAPI. Kokoro has no voice cloning: the
reference clip in a request is ignored and the voice is the one named at startup.

Started by Revia exactly as the Qwen worker is, with the same flags; the ones that
mean nothing here (--design-model, --clone-model, --attention-backend and so on) are
accepted and ignored, so one launcher serves both.
"""
from __future__ import annotations

import argparse
import hmac
import io
import json
import struct
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Callable, Iterator

SAMPLE_RATE = 24000


def pcm16_from_float(samples: Iterator[float] | list[float]) -> bytes:
    out = bytearray()
    for value in samples:
        clipped = max(-1.0, min(1.0, float(value)))
        out += struct.pack("<h", int(clipped * 32767.0))
    return bytes(out)


def wav_from_pcm16(pcm: bytes, sample_rate: int = SAMPLE_RATE) -> bytes:
    header = b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVE"
    header += b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, sample_rate, sample_rate * 2, 2, 16)
    header += b"data" + struct.pack("<I", len(pcm))
    return header + pcm


class KokoroRuntime:
    """One Kokoro pipeline, loaded on first use, behind a lock like the Qwen runtime.

    `pipeline_factory` exists for the tests: a callable returning an object that, given
    text and a voice, yields (graphemes, phonemes, audio) as kokoro.KPipeline does.
    """

    def __init__(self, args: argparse.Namespace, pipeline_factory: Callable[[], Any] | None = None) -> None:
        self.args = args
        self.lock = threading.Lock()
        self.pipeline: Any = None
        self.pipeline_factory = pipeline_factory
        self.loaded_at = 0.0
        self.last_used = 0.0
        self.device = "cpu"

    def _load(self) -> Any:
        if self.pipeline is not None:
            return self.pipeline
        started = time.perf_counter()
        if self.pipeline_factory is not None:
            self.pipeline = self.pipeline_factory()
        else:
            from kokoro import KPipeline  # imported here so --check and the tests need no model

            self.pipeline = KPipeline(lang_code=self.args.lang)
        try:
            import torch

            self.device = "cuda:0" if torch.cuda.is_available() and self.args.device != "cpu" else "cpu"
        except Exception:  # noqa: BLE001 - torch is optional for the check
            self.device = "cpu"
        self.loaded_at = time.perf_counter() - started
        return self.pipeline

    @staticmethod
    def _text(value: Any, maximum: int = 5000) -> str:
        if not isinstance(value, str) or not value.strip():
            raise ValueError("Speech text is required.")
        return value.strip()[:maximum]

    def health(self) -> dict[str, Any]:
        with self.lock:
            return {"status": "ok", "backend": "kokoro", "model_kind": "kokoro",
                    "voice": self.args.voice, "device": self.device,
                    "model_resident": self.pipeline is not None}

    def synthesize_pcm(self, request: dict[str, Any], received: float | None = None) -> tuple[dict[str, Any], bytes]:
        started = received or time.perf_counter()
        text = self._text(request.get("text"))
        with self.lock:
            model_started = time.perf_counter()
            pipeline = self._load()
            model_ready_ms = (time.perf_counter() - model_started) * 1000.0
            generation_started = time.perf_counter()
            pcm = bytearray()
            for _graphemes, _phonemes, audio in pipeline(text, voice=self.args.voice, speed=self.args.speed):
                pcm += pcm16_from_float(audio.tolist() if hasattr(audio, "tolist") else audio)
            generation_ms = (time.perf_counter() - generation_started) * 1000.0
            self.last_used = time.time()
        duration_ms = len(pcm) / 2 * 1000.0 / SAMPLE_RATE
        metadata = {
            "succeeded": True, "message": f"Speech synthesized with Kokoro ({self.args.voice}).",
            "elapsed_ms": (time.perf_counter() - started) * 1000.0, "worker_queue_wait_ms": 0.0,
            "model_ready_ms": model_ready_ms, "clone_prompt_ms": 0.0, "clone_prompt_cached": True,
            "generation_ms": generation_ms, "first_audio_chunk_ms": generation_ms, "wav_write_ms": 0.0,
            "sample_rate": SAMPLE_RATE, "audio_duration_ms": duration_ms, "device": self.device,
            "device_name": "Kokoro", "dtype": "float32", "attention_backend": "n/a", "input_mode": "complete",
            "audio_cache_hit": False, "true_incremental_audio": False, "backend": "kokoro",
            "cuda_graph": False, "talker_graph": False, "model_resident": model_ready_ms < 1.0,
            "real_time_factor": generation_ms / duration_ms if duration_ms > 0 else -1.0,
            "streaming": "whole",
        }
        return metadata, bytes(pcm)

    def synthesize_chunks(self, request: dict[str, Any], received: float | None = None,
                          chunk_ms: int = 100) -> tuple[dict[str, Any], Iterator[bytes]]:
        metadata, pcm = self.synthesize_pcm(request, received)
        frame_bytes = max(2, (SAMPLE_RATE * chunk_ms // 1000) * 2)

        def chunks() -> Iterator[bytes]:
            for start in range(0, len(pcm), frame_bytes):
                yield pcm[start:start + frame_bytes]

        return metadata, chunks()


def make_handler(runtime: KokoroRuntime, token: str) -> type[BaseHTTPRequestHandler]:
    expected = f"Bearer {token}".encode("utf-8")

    class Handler(BaseHTTPRequestHandler):
        server_version = "ReviaKokoroTTS/1.0"

        def log_message(self, format_string: str, *args: Any) -> None:
            print(f"[Kokoro HTTP] {format_string % args}", flush=True)

        def _authorized(self) -> bool:
            supplied = self.headers.get("Authorization", "").encode("utf-8", "surrogateescape")
            return hmac.compare_digest(supplied, expected)

        def _send(self, status: int, payload: dict[str, Any]) -> None:
            body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        @staticmethod
        def _headers(metadata: dict[str, Any]) -> dict[str, str]:
            names = {
                "X-Revia-Elapsed-Ms": "elapsed_ms", "X-Revia-Queue-Ms": "worker_queue_wait_ms",
                "X-Revia-Model-Ready-Ms": "model_ready_ms", "X-Revia-Prompt-Ms": "clone_prompt_ms",
                "X-Revia-Generation-Ms": "generation_ms", "X-Revia-Wav-Write-Ms": "wav_write_ms",
                "X-Revia-Audio-Duration-Ms": "audio_duration_ms", "X-Revia-Sample-Rate": "sample_rate",
                "X-Revia-Device": "device", "X-Revia-Device-Name": "device_name", "X-Revia-Dtype": "dtype",
                "X-Revia-Attention": "attention_backend", "X-Revia-Input-Mode": "input_mode",
                "X-Revia-Backend": "backend", "X-Revia-Real-Time-Factor": "real_time_factor",
            }
            out = {name: str(metadata.get(key, "")) for name, key in names.items()}
            out["X-Revia-Prompt-Cached"] = "1"
            out["X-Revia-Audio-Cache-Hit"] = "0"
            out["X-Revia-Model-Resident"] = "1" if metadata.get("model_resident") else "0"
            out["X-Revia-Cuda-Graph"] = "0"
            out["X-Revia-Talker-Graph"] = "0"
            return out

        def do_GET(self) -> None:  # noqa: N802
            if not self._authorized():
                self._send(401, {"succeeded": False, "message": "Unauthorized."})
                return
            if self.path != "/health":
                self._send(404, {"succeeded": False, "message": "Unknown endpoint."})
                return
            self._send(200, runtime.health())

        def do_POST(self) -> None:  # noqa: N802
            if not self._authorized():
                self._send(401, {"succeeded": False, "message": "Unauthorized."})
                return
            try:
                received = time.perf_counter()
                length = int(self.headers.get("Content-Length", "0"))
                if length <= 0 or length > 64 * 1024:
                    raise ValueError("The request body is empty or too large.")
                request = json.loads(self.rfile.read(length).decode("utf-8"))
                if not isinstance(request, dict):
                    raise ValueError("The request must be a JSON object.")
                if self.path in ("/prepare", "/prepare-voice", "/release"):
                    # No voice prompt to prepare and nothing to release: the model is
                    # small enough to keep. Answered so the launcher's handshake passes.
                    self._send(200, {"succeeded": True, "message": "Kokoro needs no preparation.",
                                     "voice": runtime.args.voice})
                    return
                if self.path == "/v1/audio/speech":
                    metadata, pcm = runtime.synthesize_pcm(request, received)
                    output = Path(str(request.get("output_path", ""))).expanduser()
                    if not output.suffix.lower() == ".wav":
                        raise ValueError("output_path must name a .wav file.")
                    output.parent.mkdir(parents=True, exist_ok=True)
                    output.write_bytes(wav_from_pcm16(pcm))
                    metadata["output_path"] = str(output)
                    self._send(200, metadata)
                    return
                if self.path == "/v1/audio/pcm":
                    metadata, pcm = runtime.synthesize_pcm(request, received)
                    wav = wav_from_pcm16(pcm)
                    self.send_response(200)
                    self.send_header("Content-Type", "audio/wav")
                    self.send_header("Content-Length", str(len(wav)))
                    for name, value in self._headers(metadata).items():
                        self.send_header(name, value)
                    self.end_headers()
                    self.wfile.write(wav)
                    return
                if self.path == "/v1/audio/pcm-stream":
                    metadata, chunks = runtime.synthesize_chunks(request, received)
                    self.send_response(200)
                    self.send_header("Content-Type", "audio/pcm")
                    self.send_header("Connection", "close")
                    self.send_header("X-Revia-Channels", "1")
                    self.send_header("X-Revia-Bits-Per-Sample", "16")
                    self.send_header("X-Revia-Streaming", "whole")
                    for name, value in self._headers(metadata).items():
                        self.send_header(name, value)
                    self.end_headers()
                    for chunk in chunks:
                        self.wfile.write(chunk)
                        self.wfile.flush()
                    self.close_connection = True
                    return
                if self.path in ("/v1/voice-design", "/v1/vocalizations", "/v1/audio/pcm-batch"):
                    self._send(200, {"succeeded": False,
                                     "message": "Kokoro cannot design a voice, render a clip bank or batch."})
                    return
                self._send(404, {"succeeded": False, "message": "Unknown endpoint."})
            except ValueError as error:
                self._send(400, {"succeeded": False, "message": str(error)})
            except Exception as error:  # noqa: BLE001 - reported to the client, never fatal
                self._send(500, {"succeeded": False, "message": f"Kokoro failed: {error}"})

    return Handler


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Kokoro fallback voice worker for Revia.")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8097)
    parser.add_argument("--token", required=True)
    parser.add_argument("--device", default="auto")
    parser.add_argument("--voice", default="af_heart", help="a Kokoro voice id")
    parser.add_argument("--lang", default=None,
                        help="Kokoro language code; by default the voice id's first letter "
                             "(a = American English, b = British, j = Japanese, z = Mandarin)")
    parser.add_argument("--speed", type=float, default=1.0)
    # Accepted for launcher compatibility with the Qwen worker; they mean nothing here.
    for ignored in ("--minimum-free-vram-mib", "--cpu-threads", "--max-audio-mib", "--design-model",
                    "--clone-model", "--attention-backend", "--input-mode"):
        parser.add_argument(ignored, default=None)
    for flag in ("--low-latency", "--cuda-graph", "--talker-graph"):
        parser.add_argument(flag, action="store_true")
    args = parser.parse_args(argv)
    if not args.lang:
        # Kokoro names its voices <lang><gender>_<name>, so the id already says which
        # G2P it needs; asking for it twice is how the two would disagree.
        args.lang = args.voice[:1] if args.voice else "a"
    return args


def main() -> int:
    args = parse_args()
    runtime = KokoroRuntime(args)
    server = ThreadingHTTPServer((args.host, args.port), make_handler(runtime, args.token))
    print(f"[Kokoro] listening on {args.host}:{args.port} with voice {args.voice}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
