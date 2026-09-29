from __future__ import annotations

import http.client
import importlib.util
import json
import struct
import sys
import tempfile
import threading
import unittest
import wave
from http.server import ThreadingHTTPServer
from pathlib import Path

SERVICE = Path(__file__).resolve().parents[1] / "Tools" / "speaker_id_service.py"
SPEC = importlib.util.spec_from_file_location("revia_speaker_id_service", SERVICE)
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules["revia_speaker_id_service"] = MODULE
SPEC.loader.exec_module(MODULE)


class FakeExtractor:
    """Answers with the mean and the peak of the samples, so two files differ."""

    dim = 3

    def __init__(self) -> None:
        self.calls: list[tuple[int, int]] = []

    def compute(self, samples: list[float], rate: int) -> list[float]:
        self.calls.append((len(samples), rate))
        return [sum(samples) / len(samples), max(samples), 1.0]


def write_wav(path: Path, seconds: float, rate: int = 16000, value: int = 1000, channels: int = 1) -> None:
    frames = int(seconds * rate)
    with wave.open(str(path), "wb") as handle:
        handle.setnchannels(channels)
        handle.setsampwidth(2)
        handle.setframerate(rate)
        handle.writeframes(struct.pack(f"<{frames * channels}h", *([value] * (frames * channels))))


class SpeakerIdContractTests(unittest.TestCase):
    def setUp(self) -> None:
        self.extractor = FakeExtractor()
        args = MODULE.parse_args(["--token", "t", "--model", "fake.onnx"])
        self.runtime = MODULE.SpeakerRuntime(args, extractor_factory=lambda: self.extractor)
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), MODULE.make_handler(self.runtime, "t"))
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.root = tempfile.TemporaryDirectory()

    def tearDown(self) -> None:
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=5)
        self.root.cleanup()

    def request(self, method: str, path: str, body: dict | None = None, token: str = "t"):
        connection = http.client.HTTPConnection("127.0.0.1", self.server.server_address[1], timeout=5)
        connection.request(method, path, json.dumps(body) if body is not None else None,
                           {"Authorization": f"Bearer {token}", "Content-Type": "application/json"})
        response = connection.getresponse()
        payload = json.loads(response.read())
        connection.close()
        return response.status, payload

    def test_an_utterance_becomes_a_unit_embedding(self) -> None:
        status, health = self.request("GET", "/health")
        self.assertEqual(status, 200)
        self.assertEqual(health["backend"], "sherpa-onnx")
        self.assertFalse(health["model_resident"])
        self.assertEqual(self.request("GET", "/health", token="nope")[0], 401)
        path = Path(self.root.name) / "utterance.wav"
        write_wav(path, 1.5)
        status, payload = self.request("POST", "/v1/speaker/embed", {"wav_path": str(path)})
        self.assertEqual(status, 200)
        self.assertTrue(payload["succeeded"])
        self.assertEqual(payload["dimension"], 3)
        self.assertAlmostEqual(sum(v * v for v in payload["embedding"]), 1.0, places=5)
        self.assertAlmostEqual(payload["duration_ms"], 1500.0)
        self.assertEqual(self.extractor.calls, [(24000, 16000)])
        self.assertTrue(self.request("GET", "/health")[1]["model_resident"])

    def test_stereo_is_averaged_and_bad_audio_is_refused_as_an_answer(self) -> None:
        stereo = Path(self.root.name) / "stereo.wav"
        write_wav(stereo, 1.0, channels=2)
        status, payload = self.request("POST", "/v1/speaker/embed", {"wav_path": str(stereo)})
        self.assertEqual(status, 200)
        self.assertEqual(self.extractor.calls[-1], (16000, 16000))
        short = Path(self.root.name) / "short.wav"
        write_wav(short, 0.2)
        status, payload = self.request("POST", "/v1/speaker/embed", {"wav_path": str(short)})
        self.assertEqual(status, 400)
        self.assertIn("half a second", payload["message"])
        wrong_rate = Path(self.root.name) / "rate.wav"
        write_wav(wrong_rate, 1.0, rate=44100)
        status, payload = self.request("POST", "/v1/speaker/embed", {"wav_path": str(wrong_rate)})
        self.assertEqual(status, 400)
        self.assertIn("16000 Hz", payload["message"])
        status, payload = self.request("POST", "/v1/speaker/embed", {"wav_path": str(Path(self.root.name) / "missing.wav")})
        self.assertEqual(status, 400)
        self.assertEqual(self.request("POST", "/v1/other", {})[0], 404)


if __name__ == "__main__":
    unittest.main()
