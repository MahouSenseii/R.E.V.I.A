from __future__ import annotations

import argparse
import http.client
import importlib.util
import json
import tempfile
import threading
import types
import unittest
from http.server import ThreadingHTTPServer
from pathlib import Path
from unittest.mock import patch


SERVICE = Path(__file__).resolve().parents[1] / "Tools" / "revia_image_service.py"
SPEC = importlib.util.spec_from_file_location("revia_image_service", SERVICE)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class FakeRuntime:
    def __init__(self) -> None:
        self.requests: list[dict] = []

    def describe(self) -> dict:
        return {"loaded": False}

    def generate(self, payload: dict) -> dict:
        self.requests.append(payload)
        return {"ok": True}


class ImageHandlerTests(unittest.TestCase):
    TOKEN = "image-test-token"

    def setUp(self) -> None:
        self.runtime = FakeRuntime()
        self.server = ThreadingHTTPServer(
            ("127.0.0.1", 0), MODULE.build_handler(self.runtime, self.TOKEN))
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def tearDown(self) -> None:
        self.server.shutdown()
        self.server.server_close()

    def post(self, body: bytes, headers: dict[str, str]) -> tuple[int, dict]:
        connection = http.client.HTTPConnection(
            "127.0.0.1", self.server.server_address[1], timeout=5)
        try:
            connection.putrequest("POST", "/generate")
            for name, value in headers.items():
                connection.putheader(name, value)
            connection.endheaders()
            connection.send(body)
            response = connection.getresponse()
            return response.status, json.loads(response.read() or b"{}")
        finally:
            connection.close()

    def authorised(self, length: str) -> dict[str, str]:
        return {"Authorization": f"Bearer {self.TOKEN}", "Content-Length": length}

    def test_a_wrong_or_non_ascii_key_is_refused_cleanly(self) -> None:
        body = json.dumps({"prompt": "a cat", "outputPath": "x.png"}).encode()
        for authorization in ("Bearer wrong", "Bearer imäge-test-token", ""):
            status, _ = self.post(body, {
                "Authorization": authorization.encode("utf-8").decode("latin-1"),
                "Content-Length": str(len(body))})
            self.assertEqual(status, 401, authorization)
        self.assertEqual(self.runtime.requests, [])

    def test_the_body_length_is_bounded_before_it_is_read(self) -> None:
        status, _ = self.post(b"", self.authorised(str(10 * 1024 * 1024)))
        self.assertEqual(status, 413)
        status, _ = self.post(b"", self.authorised("not-a-number"))
        self.assertEqual(status, 400)
        self.assertEqual(self.runtime.requests, [])

    def test_only_a_json_object_reaches_the_runtime(self) -> None:
        body = b"[1, 2, 3]"
        status, _ = self.post(body, self.authorised(str(len(body))))
        self.assertEqual(status, 400)
        body = json.dumps({"prompt": "a cat", "outputPath": "x.png"}).encode()
        status, reply = self.post(body, self.authorised(str(len(body))))
        self.assertEqual((status, reply.get("ok")), (200, True))
        self.assertEqual(len(self.runtime.requests), 1)


class ImageRuntimeTests(unittest.TestCase):
    def runtime(self):
        return MODULE.ImageRuntime(argparse.Namespace(
            model="unused", device="cpu", min_free_vram_mib=0, steps=1, guidance=1.0,
            width=256, height=256, cache_dir="", offline=True))

    def test_a_missing_destination_is_refused_before_the_model_loads(self) -> None:
        runtime = self.runtime()

        def refuse_to_load() -> dict:
            raise AssertionError("the model loaded for a request that could not be saved")

        runtime.ensure_loaded = refuse_to_load
        for payload in ({"prompt": "a cat"}, {"prompt": "a cat", "outputPath": ""},
                        {"prompt": "a cat", "outputPath": 7}):
            with self.assertRaisesRegex(ValueError, "outputPath"):
                runtime.generate(payload)

    def test_bad_generation_options_are_refused_before_the_model_loads(self) -> None:
        runtime = self.runtime()

        def refuse_to_load() -> dict:
            raise AssertionError("the model loaded for invalid generation options")

        runtime.ensure_loaded = refuse_to_load
        for option, value in (("steps", "bad"), ("width", None), ("height", []),
                              ("seed", "bad"), ("guidance", "bad"),
                              ("guidance", float("nan")), ("guidance", float("inf"))):
            with self.subTest(option=option, value=value):
                with self.assertRaises(ValueError):
                    runtime.generate({"prompt": "a cat", "outputPath": "unused.png",
                                      option: value})

    def test_unload_waits_until_a_loaded_pipeline_has_generated(self) -> None:
        runtime = self.runtime()
        ready = threading.Event()
        unloading = threading.Event()
        unloaded = threading.Event()
        original_load = runtime.ensure_loaded

        class Image:
            def save(self, output, format):
                Path(output).write_bytes(b"generated image")

        runtime.pipeline = lambda **options: types.SimpleNamespace(images=[Image()])

        def load_then_allow_unload():
            state = original_load()
            ready.set()
            self.assertTrue(unloading.wait(2), "unload did not attempt to start")
            # The old implementation unloads in this gap; the fixed implementation
            # keeps inference ownership and lets unload finish afterward.
            unloaded.wait(0.1)
            return state

        runtime.ensure_loaded = load_then_allow_unload

        def release_model():
            ready.wait(2)
            unloading.set()
            runtime.unload()
            unloaded.set()

        thread = threading.Thread(target=release_model, daemon=True)
        fake_torch = types.SimpleNamespace(cuda=types.SimpleNamespace(is_available=lambda: False))
        with patch.dict("sys.modules", {"torch": fake_torch}):
            thread.start()
            try:
                with tempfile.TemporaryDirectory() as directory:
                    output = Path(directory) / "image.png"
                    reply = runtime.generate({"prompt": "a cat", "outputPath": str(output)})
                    self.assertTrue(reply["ok"])
                    self.assertEqual(output.read_bytes(), b"generated image")
            finally:
                thread.join(2)
        self.assertTrue(unloaded.is_set(), "unload did not finish after generation")
        self.assertIsNone(runtime.pipeline)


if __name__ == "__main__":
    unittest.main()
