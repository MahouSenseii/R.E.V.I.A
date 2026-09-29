#!/usr/bin/env python3
"""Speaker embeddings for Revia, on the same HTTP-worker contract as her other workers.

One utterance in (a 16 kHz mono 16-bit WAV, the same file whisper just transcribed),
one embedding out: a unit vector that says who was speaking, from sherpa-onnx's
speaker-embedding extractor (Apache-2.0) and a WeSpeaker or 3D-Speaker model file.
Nothing here decides who anyone is: the matching, the enrolment and the consent live
in Revia, and the audio never leaves the machine. Bearer-authenticated, loopback only,
CPU by default -- an embedding takes tens of milliseconds.

    python speaker_id_service.py --port 8098 --token <t> --model Models/wespeaker_en_voxceleb_CAM++.onnx
"""
from __future__ import annotations

import argparse
import hmac
import json
import struct
import sys
import threading
import time
import wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Callable

EXPECTED_SAMPLE_RATE = 16000


def read_wav(path: Path) -> tuple[list[float], int]:
    """A WAV as float samples in [-1, 1] and its rate; stereo is averaged to mono."""
    with wave.open(str(path), "rb") as handle:
        channels = handle.getnchannels()
        width = handle.getsampwidth()
        rate = handle.getframerate()
        frames = handle.readframes(handle.getnframes())
    if width != 2:
        raise ValueError(f"{path.name} is {width * 8}-bit; 16-bit PCM is expected.")
    count = len(frames) // 2
    values = struct.unpack(f"<{count}h", frames[:count * 2])
    if channels > 1:
        values = [sum(values[i:i + channels]) / channels for i in range(0, len(values) - channels + 1, channels)]
    return [v / 32768.0 for v in values], rate


class SpeakerRuntime:
    """One extractor, loaded on first use, behind a lock.

    `extractor_factory` exists for the tests: a callable returning an object with
    `compute(samples, rate) -> list[float]` and a `dim` attribute.
    """

    def __init__(self, args: argparse.Namespace, extractor_factory: Callable[[], Any] | None = None) -> None:
        self.args = args
        self.lock = threading.Lock()
        self.extractor: Any = None
        self.extractor_factory = extractor_factory
        self.embeddings = 0

    def _load(self) -> Any:
        if self.extractor is not None:
            return self.extractor
        if self.extractor_factory is not None:
            self.extractor = self.extractor_factory()
            return self.extractor
        import sherpa_onnx  # imported here so --check and the tests need no model

        config = sherpa_onnx.SpeakerEmbeddingExtractorConfig(
            model=str(self.args.model), num_threads=max(1, int(self.args.threads)), provider=self.args.provider)
        if not config.validate():
            raise RuntimeError(f"The speaker model at {self.args.model} could not be loaded.")
        extractor = sherpa_onnx.SpeakerEmbeddingExtractor(config)

        class Wrapped:
            dim = extractor.dim

            @staticmethod
            def compute(samples: list[float], rate: int) -> list[float]:
                stream = extractor.create_stream()
                stream.accept_waveform(sample_rate=rate, waveform=samples)
                stream.input_finished()
                if not extractor.is_ready(stream):
                    raise ValueError("The utterance is too short for an embedding.")
                return list(extractor.compute(stream))

        self.extractor = Wrapped()
        return self.extractor

    def health(self) -> dict[str, Any]:
        with self.lock:
            return {"status": "ok", "backend": "sherpa-onnx", "model": Path(str(self.args.model)).name,
                    "model_resident": self.extractor is not None,
                    "dimension": getattr(self.extractor, "dim", 0) if self.extractor is not None else 0,
                    "embeddings": self.embeddings}

    def embed(self, wav_path: str) -> dict[str, Any]:
        path = Path(wav_path)
        if not path.is_file():
            raise ValueError(f"No WAV at {wav_path}.")
        samples, rate = read_wav(path)
        if rate != EXPECTED_SAMPLE_RATE:
            raise ValueError(f"{path.name} is {rate} Hz; {EXPECTED_SAMPLE_RATE} Hz is expected.")
        duration_ms = len(samples) * 1000.0 / rate
        if duration_ms < 500:
            raise ValueError("The utterance is shorter than half a second.")
        started = time.perf_counter()
        with self.lock:
            extractor = self._load()
            vector = [float(v) for v in extractor.compute(samples, rate)]
            self.embeddings += 1
        norm = sum(v * v for v in vector) ** 0.5
        if norm > 0:
            vector = [v / norm for v in vector]
        return {"succeeded": True, "embedding": vector, "dimension": len(vector), "duration_ms": duration_ms,
                "elapsed_ms": (time.perf_counter() - started) * 1000.0, "backend": "sherpa-onnx"}


def make_handler(runtime: SpeakerRuntime, token: str) -> type[BaseHTTPRequestHandler]:
    expected = f"Bearer {token}".encode("utf-8")

    class Handler(BaseHTTPRequestHandler):
        server_version = "ReviaSpeakerId/1.0"

        def log_message(self, format: str, *args: Any) -> None:  # noqa: A002
            sys.stderr.write("[SpeakerId HTTP] " + format % args + "\n")

        def _authorized(self) -> bool:
            header = (self.headers.get("Authorization") or "").encode("utf-8")
            return hmac.compare_digest(header, expected)

        def _send(self, status: int, payload: dict[str, Any]) -> None:
            body = json.dumps(payload).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self) -> None:  # noqa: N802
            if not self._authorized():
                self._send(401, {"succeeded": False, "message": "Unauthorized."})
                return
            if self.path == "/health":
                self._send(200, runtime.health())
                return
            self._send(404, {"succeeded": False, "message": "Unknown endpoint."})

        def do_POST(self) -> None:  # noqa: N802
            if not self._authorized():
                self._send(401, {"succeeded": False, "message": "Unauthorized."})
                return
            try:
                length = int(self.headers.get("Content-Length") or 0)
                request = json.loads(self.rfile.read(length) or b"{}")
                if self.path == "/v1/speaker/embed":
                    self._send(200, runtime.embed(str(request.get("wav_path", ""))))
                    return
                self._send(404, {"succeeded": False, "message": "Unknown endpoint."})
            except ValueError as error:
                self._send(400, {"succeeded": False, "message": str(error)})
            except Exception as error:  # noqa: BLE001 - reported to the client, never fatal
                self._send(500, {"succeeded": False, "message": f"Speaker embedding failed: {error}"})

    return Handler


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Speaker embedding worker for Revia.")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8098)
    parser.add_argument("--token", required=True)
    parser.add_argument("--model", default="Models/wespeaker_en_voxceleb_CAM++.onnx")
    parser.add_argument("--provider", default="cpu")
    parser.add_argument("--threads", type=int, default=2)
    return parser.parse_args(argv)


def main() -> int:
    args = parse_args()
    runtime = SpeakerRuntime(args)
    server = ThreadingHTTPServer((args.host, args.port), make_handler(runtime, args.token))
    print(f"[SpeakerId] listening on {args.host}:{args.port} with {Path(args.model).name}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
