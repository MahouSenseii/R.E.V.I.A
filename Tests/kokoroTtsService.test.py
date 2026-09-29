from __future__ import annotations

import http.client
import importlib.util
import json
import sys
import tempfile
import threading
import unittest
from http.server import ThreadingHTTPServer
from pathlib import Path

SERVICE = Path(__file__).resolve().parents[1] / "Tools" / "kokoro_tts_service.py"
SPEC = importlib.util.spec_from_file_location("revia_kokoro_tts_service", SERVICE)
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules["revia_kokoro_tts_service"] = MODULE
SPEC.loader.exec_module(MODULE)


class FakePipeline:
    """Yields two segments of audio, like kokoro.KPipeline does."""

    def __init__(self) -> None:
        self.calls: list[tuple[str, str, float]] = []

    def __call__(self, text: str, voice: str, speed: float = 1.0):
        self.calls.append((text, voice, speed))
        yield "graphemes", "phonemes", [0.0, 0.5, -0.5]
        yield "graphemes", "phonemes", [1.0]


class KokoroContractTests(unittest.TestCase):
    def setUp(self) -> None:
        self.pipeline = FakePipeline()
        args = MODULE.parse_args(["--token", "t", "--voice", "af_heart", "--device", "cpu",
                                  "--clone-model", "ignored", "--low-latency"])
        self.runtime = MODULE.KokoroRuntime(args, pipeline_factory=lambda: self.pipeline)
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), MODULE.make_handler(self.runtime, "t"))
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def tearDown(self) -> None:
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=5)

    def request(self, method: str, path: str, body: dict | None = None, token: str = "t"):
        connection = http.client.HTTPConnection("127.0.0.1", self.server.server_address[1], timeout=5)
        connection.request(method, path, json.dumps(body) if body is not None else None,
                           {"Authorization": f"Bearer {token}", "Content-Type": "application/json"})
        response = connection.getresponse()
        payload = response.read()
        connection.close()
        return response, payload

    def test_the_launcher_flags_are_accepted_and_health_names_the_backend(self) -> None:
        response, payload = self.request("GET", "/health")
        self.assertEqual(response.status, 200)
        health = json.loads(payload)
        self.assertEqual(health["backend"], "kokoro")
        self.assertEqual(health["voice"], "af_heart")
        self.assertFalse(health["model_resident"])
        wrong, _ = self.request("GET", "/health", token="nope")
        self.assertEqual(wrong.status, 401)

    def test_pcm_is_a_wav_with_the_timing_headers_and_the_reference_is_ignored(self) -> None:
        response, wav = self.request("POST", "/v1/audio/pcm", {
            "text": "Hello there.", "reference_audio": "/nowhere.wav", "reference_text": "ignored"})
        self.assertEqual(response.status, 200)
        self.assertEqual(response.getheader("Content-Type"), "audio/wav")
        self.assertEqual(response.getheader("X-Revia-Sample-Rate"), "24000")
        self.assertEqual(response.getheader("X-Revia-Backend"), "kokoro")
        self.assertEqual(response.getheader("X-Revia-Device-Name"), "Kokoro")
        self.assertEqual(wav[:4], b"RIFF")
        self.assertEqual(int.from_bytes(wav[40:44], "little"), 8)
        self.assertEqual(wav[44:52], b"\x00\x00\xff\x3f\x01\xc0\xff\x7f")
        self.assertEqual(self.pipeline.calls, [("Hello there.", "af_heart", 1.0)])
        health = json.loads(self.request("GET", "/health")[1])
        self.assertTrue(health["model_resident"])

    def test_the_stream_and_the_file_endpoints_carry_the_same_audio(self) -> None:
        response, pcm = self.request("POST", "/v1/audio/pcm-stream", {"text": "Hi."})
        self.assertEqual(response.status, 200)
        self.assertEqual(response.getheader("Content-Type"), "audio/pcm")
        self.assertIsNone(response.getheader("Content-Length"))
        self.assertEqual(response.getheader("X-Revia-Streaming"), "whole")
        self.assertEqual(pcm, b"\x00\x00\xff\x3f\x01\xc0\xff\x7f")
        with tempfile.TemporaryDirectory() as root:
            target = Path(root) / "out" / "phrase.wav"
            response, payload = self.request("POST", "/v1/audio/speech", {"text": "Hi.", "output_path": str(target)})
            self.assertEqual(response.status, 200)
            self.assertTrue(json.loads(payload)["succeeded"])
            self.assertEqual(target.read_bytes()[44:], pcm)

    def test_what_kokoro_cannot_do_is_refused_as_an_answer_not_an_error(self) -> None:
        response, payload = self.request("POST", "/v1/voice-design", {"text": "x"})
        self.assertEqual(response.status, 200)
        self.assertFalse(json.loads(payload)["succeeded"])
        prepared, payload = self.request("POST", "/prepare-voice", {"reference_audio": "x"})
        self.assertEqual(prepared.status, 200)
        self.assertTrue(json.loads(payload)["succeeded"])
        bad, payload = self.request("POST", "/v1/audio/pcm", {"text": "   "})
        self.assertEqual(bad.status, 400)


if __name__ == "__main__":
    unittest.main()
