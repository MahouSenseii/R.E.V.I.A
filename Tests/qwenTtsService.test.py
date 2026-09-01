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
    cuda_math_mode = "default"
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


class _FakeMatmul:
    allow_tf32 = False


class _FakeCudaBackend:
    matmul = _FakeMatmul()


class _FakeCudnnBackend:
    allow_tf32 = False


class _FakeBackends:
    cuda = _FakeCudaBackend()
    cudnn = _FakeCudnnBackend()


class _FakeCuda:
    def __init__(self, major: int) -> None:
        self.major = major

    def get_device_capability(self, index: int) -> tuple[int, int]:
        del index
        return self.major, 0


class _FakeTorch:
    def __init__(self, major: int) -> None:
        self.cuda = _FakeCuda(major)
        self.backends = _FakeBackends()
        self.precision = "highest"

    def set_float32_matmul_precision(self, precision: str) -> None:
        self.precision = precision


class CudaMathPolicyTests(unittest.TestCase):
    def test_ampere_enables_tf32_for_residual_float32_inference(self) -> None:
        torch = _FakeTorch(8)

        enabled = MODULE._enable_ampere_tf32(torch, "cuda:0")

        self.assertTrue(enabled)
        self.assertEqual(torch.precision, "high")
        self.assertTrue(torch.backends.cuda.matmul.allow_tf32)
        self.assertTrue(torch.backends.cudnn.allow_tf32)

    def test_cpu_and_turing_keep_the_default_math_path(self) -> None:
        cpu = _FakeTorch(8)
        turing = _FakeTorch(7)

        self.assertFalse(MODULE._enable_ampere_tf32(cpu, "cpu"))
        self.assertFalse(MODULE._enable_ampere_tf32(turing, "cuda:0"))
        self.assertEqual(cpu.precision, "highest")
        self.assertEqual(turing.precision, "highest")


if __name__ == "__main__":
    unittest.main()
