from __future__ import annotations

import argparse
import http.client
import importlib.util
import json
import hashlib
import tempfile
import threading
import time
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
HAS_PILLOW = importlib.util.find_spec("PIL") is not None


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

    def test_sd_turbo_uses_distilled_parameters(self) -> None:
        runtime = self.runtime()
        runtime.model_id = "stabilityai/sd-turbo"
        self.assertTrue(hasattr(runtime, "generation_options"), "Provider options are not validated")
        options = runtime.generation_options({"steps": 30, "guidance": 7.5,
                                              "negativePrompt": "blurry"})
        self.assertEqual(options["num_inference_steps"], 4)
        self.assertEqual(options["guidance_scale"], 0.0)
        self.assertIsNone(options["negative_prompt"])

    def test_standard_provider_retains_guidance_and_steps(self) -> None:
        runtime = self.runtime()
        runtime.model_id = "stabilityai/stable-diffusion-xl-base-1.0"
        self.assertTrue(hasattr(runtime, "generation_options"), "Provider options are not validated")
        options = runtime.generation_options({"steps": 30, "guidance": 7.0,
                                              "width": 1024, "height": 1024,
                                              "negativePrompt": "blurry"})
        self.assertEqual(options["num_inference_steps"], 30)
        self.assertEqual(options["guidance_scale"], 7.0)
        self.assertEqual(options["negative_prompt"], "blurry")
        self.assertEqual((options["width"], options["height"]), (1024, 1024))

    def test_explicit_weight_variant_reaches_the_provider(self) -> None:
        runtime = self.runtime()
        runtime.args.variant = "fp16"
        calls = []
        pipeline = types.SimpleNamespace(to=lambda _: pipeline,
            set_progress_bar_config=lambda **_: None, enable_attention_slicing=lambda: None)
        def load(model, **options):
            calls.append((model, options))
            return pipeline
        fake_torch = types.SimpleNamespace(float32="float32", set_num_threads=lambda _: None,
                                           cuda=types.SimpleNamespace(is_available=lambda: False))
        fake_diffusers = types.SimpleNamespace(AutoPipelineForText2Image=types.SimpleNamespace(from_pretrained=load))
        with patch.dict("sys.modules", {"torch": fake_torch, "diffusers": fake_diffusers}):
            runtime.ensure_loaded()
        self.assertEqual(calls[0][1].get("variant"), "fp16")
        self.assertTrue(calls[0][1]["local_files_only"])

    def test_auto_gpu_preserves_shared_reserve(self) -> None:
        runtime = self.runtime()
        runtime.args.device = "auto"
        runtime.args.min_free_vram_mib = 4200
        runtime.args.gpu_reserve_mib = 1536
        from contextlib import nullcontext
        fake_torch = types.SimpleNamespace(float16="float16", float32="float32", cuda=types.SimpleNamespace(
            is_available=lambda: True, device_count=lambda: 1, device=lambda _: nullcontext(),
            get_device_properties=lambda _: types.SimpleNamespace(name="busy GPU"),
            mem_get_info=lambda: (5000 * 1024 * 1024, 8192 * 1024 * 1024)))
        self.assertEqual(runtime._select_device(fake_torch)[0], "cpu")
        runtime.model_id = "stabilityai/sdxl-turbo"
        fake_torch.cuda.mem_get_info = lambda: (10000 * 1024 * 1024, 12227 * 1024 * 1024)
        self.assertEqual(runtime._select_device(fake_torch)[0], "cpu",
                         "SDXL's measured working set consumed the shared reserve")

    def test_explicit_cuda_failure_does_not_silently_run_on_cpu(self) -> None:
        runtime = self.runtime()
        runtime.args.device = "cuda:1"
        fake_torch = types.SimpleNamespace(cuda=types.SimpleNamespace(is_available=lambda: False),
                                          float32="float32")
        with self.assertRaisesRegex(RuntimeError, "CUDA"):
            runtime._select_device(fake_torch)

    def test_job_cancel_interrupts_steps_and_never_publishes(self) -> None:
        runtime = self.runtime()
        self.assertTrue(hasattr(runtime, "submit"), "Cancellable image jobs are missing")
        entered = threading.Event()
        proceed = threading.Event()

        def inference(**options):
            entered.set()
            self.assertTrue(proceed.wait(2))
            options["callback_on_step_end"](runtime.pipeline, 0, 0, {})
            raise AssertionError("Cancelled inference continued")

        runtime.pipeline = inference
        fake_torch = types.SimpleNamespace(cuda=types.SimpleNamespace(is_available=lambda: False))
        with tempfile.TemporaryDirectory() as directory, patch.dict("sys.modules", {"torch": fake_torch}):
            runtime.args.output_root = directory
            path = Path(directory) / "cancelled.png"
            job = runtime.submit({"jobId": "cancel-check", "prompt": "cat", "outputPath": str(path)})
            self.assertTrue(entered.wait(2))
            with self.assertRaisesRegex(RuntimeError, "busy"):
                runtime.submit({"jobId": "second", "prompt": "cat", "outputPath": str(path)})
            runtime.cancel(job["jobId"])
            proceed.set()
            deadline = time.monotonic() + 2
            while time.monotonic() < deadline and runtime.job_status(job["jobId"])["state"] not in ("cancelled", "failed"):
                time.sleep(0.01)
            self.assertEqual(runtime.job_status(job["jobId"])["state"], "cancelled")
            self.assertFalse(path.exists())

    def test_foreign_output_is_refused_before_loading(self) -> None:
        runtime = self.runtime()
        with tempfile.TemporaryDirectory() as directory:
            runtime.args.output_root = str(Path(directory) / "owned")
            runtime.ensure_loaded = lambda: self.fail("foreign output loaded the model")
            with self.assertRaisesRegex(ValueError, "output"):
                runtime.generate({"prompt": "cat", "outputPath": str(Path(directory) / "foreign.png")})

    @unittest.skipUnless(HAS_PILLOW, "Pillow is required for actual PNG decode checks")
    def test_generated_png_is_decoded_and_receipt_identifies_exact_bytes(self) -> None:
        from PIL import Image

        runtime = self.runtime()
        runtime.pipeline = lambda **options: types.SimpleNamespace(images=[Image.new("RGB", (256, 256), "red")])
        fake_torch = types.SimpleNamespace(cuda=types.SimpleNamespace(is_available=lambda: False))
        with tempfile.TemporaryDirectory() as directory, patch.dict("sys.modules", {"torch": fake_torch}):
            runtime.args.output_root = directory
            output = Path(directory) / "image.png"
            reply = runtime.generate({"jobId": "receipt-check", "prompt": "cat", "outputPath": str(output)})
            self.assertEqual(reply.get("sha256"), hashlib.sha256(output.read_bytes()).hexdigest())
            self.assertEqual(reply.get("jobId"), "receipt-check")
            self.assertEqual((reply["width"], reply["height"]), (256, 256))
            with self.assertRaisesRegex(ValueError, "exist"):
                runtime.generate({"prompt": "cat", "outputPath": str(output)})

    @unittest.skipUnless(HAS_PILLOW, "Pillow is required for actual PNG decode checks")
    def test_completed_job_releases_model_and_reports_actual_lifetime(self) -> None:
        from PIL import Image

        runtime = self.runtime()
        runtime.pipeline = lambda **options: types.SimpleNamespace(images=[Image.new("RGB", (256, 256), "red")])
        fake_torch = types.SimpleNamespace(cuda=types.SimpleNamespace(is_available=lambda: False))
        with tempfile.TemporaryDirectory() as directory, patch.dict("sys.modules", {"torch": fake_torch}):
            runtime.args.output_root = directory
            job = runtime.submit({"jobId": "release-check", "prompt": "cat", "outputPath": str(Path(directory) / "image.png")})
            deadline = time.monotonic() + 2
            while runtime.active_job is not None and time.monotonic() < deadline:
                time.sleep(0.01)
            self.assertIsNone(runtime.pipeline)
            result = runtime.job_status(job["jobId"])
            self.assertEqual(result["state"], "succeeded")
            self.assertFalse(result["loaded"], "Job receipt claims released weights remain loaded")

    @unittest.skipUnless(HAS_PILLOW, "Pillow is required for actual PNG decode checks")
    def test_corrupt_or_wrong_size_generation_is_never_published(self) -> None:
        from PIL import Image

        class CorruptImage:
            def save(self, output, format):
                Path(output).write_bytes(b"not a PNG")

        fake_torch = types.SimpleNamespace(cuda=types.SimpleNamespace(is_available=lambda: False))
        for generated in (CorruptImage(), Image.new("RGB", (8, 8), "red")):
            with tempfile.TemporaryDirectory() as directory, patch.dict("sys.modules", {"torch": fake_torch}):
                runtime = self.runtime()
                runtime.args.output_root = directory
                runtime.pipeline = lambda **options: types.SimpleNamespace(images=[generated])
                output = Path(directory) / "image.png"
                with self.assertRaises((ValueError, OSError)):
                    runtime.generate({"prompt": "cat", "outputPath": str(output)})
                self.assertFalse(output.exists())

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

    @unittest.skipUnless(HAS_PILLOW, "Pillow is required for actual PNG decode checks")
    def test_unload_waits_until_a_loaded_pipeline_has_generated(self) -> None:
        from PIL import Image
        runtime = self.runtime()
        ready = threading.Event()
        unloading = threading.Event()
        unloaded = threading.Event()
        original_load = runtime.ensure_loaded

        runtime.pipeline = lambda **options: types.SimpleNamespace(images=[Image.new("RGB", (256, 256), "red")])

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
                    self.assertTrue(output.read_bytes().startswith(b"\x89PNG"))
            finally:
                thread.join(2)
        self.assertTrue(unloaded.is_set(), "unload did not finish after generation")
        self.assertIsNone(runtime.pipeline)


if __name__ == "__main__":
    unittest.main()
