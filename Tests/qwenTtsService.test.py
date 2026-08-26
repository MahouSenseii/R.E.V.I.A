from __future__ import annotations

import http.client
import importlib.util
import json
import threading
import unittest
from http.server import ThreadingHTTPServer
from pathlib import Path


SERVICE = Path(__file__).resolve().parents[1] / "Tools" / "qwen_tts_service.py"
SPEC = importlib.util.spec_from_file_location("revia_qwen_tts_service", SERVICE)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class FakeArgs:
    input_mode = "complete"


class FakeRuntime:
    model_kind = ""
    device = "cpu"
    device_name = "CPU"
    dtype_name = "float32"
    attention_backend = "auto"
    model = None
    loaded_at = 0.0
    last_used = 0.0
    clone_prompts: dict = {}
    args = FakeArgs()

    def vocalizations(self, request: dict) -> dict:
        return {"succeeded": True, "received": request.get("sentinel")}


class QwenHandlerTests(unittest.TestCase):
    def test_vocalization_endpoint_passes_the_parsed_request(self) -> None:
        token = "test-token"
        server = ThreadingHTTPServer(
            ("127.0.0.1", 0), MODULE.make_handler(FakeRuntime(), token))
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            connection = http.client.HTTPConnection(
                "127.0.0.1", server.server_address[1], timeout=5)
            body = json.dumps({"sentinel": "request-object"})
            connection.request(
                "POST", "/v1/vocalizations", body,
                {"Authorization": f"Bearer {token}", "Content-Type": "application/json"})
            response = connection.getresponse()
            result = json.loads(response.read().decode("utf-8"))
            self.assertEqual(response.status, 200)
            self.assertEqual(result["received"], "request-object")
        finally:
            server.shutdown()
            server.server_close()
            thread.join(timeout=5)


if __name__ == "__main__":
    unittest.main()
