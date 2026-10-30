import importlib.util
import json
import hashlib
import os
from pathlib import Path
import tempfile
import types
import unittest


PATH = Path(__file__).resolve().parents[1] / "Tools" / "revia_video_worker.py"
SPEC = importlib.util.spec_from_file_location("revia_video_worker", PATH)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)
HAS_PILLOW = importlib.util.find_spec("PIL") is not None


class VideoWorkerTests(unittest.TestCase):
    def test_t2v_cpu_and_i2v_cuda_generators_preserve_the_selected_seed(self):
        calls = []
        torch = types.SimpleNamespace(Generator=lambda device: types.SimpleNamespace(
            manual_seed=lambda seed: calls.append((device, seed))))
        job = {"prompt": "neutral shapes", "seed": 42}
        MODULE.inference_arguments(job, torch, None)
        source = object()
        arguments = MODULE.inference_arguments(job, torch, source)
        self.assertEqual(calls, [("cpu", 42), ("cuda:1", 42)])
        self.assertIs(arguments["image"], source)
        self.assertEqual((arguments["num_frames"], arguments["num_inference_steps"], arguments["frame_rate"]), (49, 50, 25))
        self.assertNotIn("strength", arguments)
    def test_unknown_mode_and_missing_image_are_refused_before_model_loading(self):
        for patch in ({"mode": "foreign"}, {"mode": "image_to_video"},
                      {"mode": "text_to_video", "source": {"path": "elsewhere"}}):
            job = {"mode": "text_to_video", "prompt": "a neutral moving shape", "seed": 42,
                   "width": 384, "height": 256, "frameCount": 49, "fps": 25, "steps": 50}
            job.update(patch)
            with self.assertRaises(ValueError):
                MODULE.validate_request(job)

    def test_only_qualified_geometry_and_bounded_seed_are_admitted(self):
        base = {"mode": "text_to_video", "prompt": "a neutral moving shape", "seed": 42,
                "width": 384, "height": 256, "frameCount": 49, "fps": 25, "steps": 50}
        MODULE.validate_request(base)
        for key, value in (("width", 512), ("height", 384), ("frameCount", 48), ("fps", 30), ("steps", 10),
                           ("seed", -1), ("seed", True), ("prompt", "x" * 8193)):
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                MODULE.validate_request(dict(base, **{key: value}))

    @unittest.skipUnless(HAS_PILLOW, "Pillow is required for actual reference decode/preparation")
    def test_source_bytes_metadata_and_fixed_path_are_validated_and_prepared(self):
        from PIL import Image

        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary).absolute()
            source = directory / "source.bin"
            Image.new("RGB", (80, 40), "blue").save(source, format="PNG")
            metadata = {"path": "source.bin", "id": "source-id", "sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                        "mimeType": "image/png", "width": 80, "height": 40}
            job = {"directory": str(directory), "source": metadata}
            prepared, value = MODULE.prepare_source(job)
            self.assertEqual(prepared.size, (384, 256))
            self.assertEqual(prepared.getpixel((192, 128)), (0, 0, 255))
            self.assertEqual(prepared.getpixel((0, 0)), (0, 0, 0))
            self.assertEqual(value, hashlib.sha256(prepared.tobytes()).hexdigest())
            for key, invalid in (("path", "../source.bin"), ("sha256", "0" * 64), ("width", 81), ("mimeType", "image/jpeg")):
                with self.subTest(key=key), self.assertRaises(ValueError):
                    MODULE.prepare_source(dict(job, source=dict(metadata, **{key: invalid})))

    def test_job_root_and_output_are_not_model_selected_paths(self):
        with tempfile.TemporaryDirectory() as temporary:
            temporary = Path(temporary).absolute()
            root = temporary / "outputs"
            root.mkdir()
            scratch = temporary / "ReviaVideoJobs"
            directory = scratch / "job-a"
            directory.mkdir(parents=True)
            job = {"jobId": "job-a", "mode": "text_to_video", "prompt": "neutral shapes", "seed": 42,
                   "width": 384, "height": 256, "frameCount": 49, "fps": 25, "steps": 50,
                   "outputRoot": str(root), "outputPath": str(root / "video-job-a.mp4"),
                   "scratchRoot": str(scratch), "jobDirectory": str(directory)}
            path = directory / "job.json"
            path.write_text(json.dumps(job))
            self.assertEqual(MODULE.load_job(path)["jobId"], "job-a")
            for key, invalid in (("scratchRoot", str(root)), ("jobDirectory", str(directory.parent)),
                                 ("outputRoot", str(scratch))):
                with self.subTest(key=key):
                    path.write_text(json.dumps(dict(job, **{key: invalid})))
                    with self.assertRaises(ValueError):
                        MODULE.load_job(path)
            job["outputPath"] = str(root.parent / "foreign.mp4")
            path.write_text(json.dumps(job))
            with self.assertRaises(ValueError):
                MODULE.load_job(path)

    def test_publication_is_bounded_exclusive_and_does_not_depend_on_hard_links(self):
        from unittest.mock import patch
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).absolute()
            pending = root / "pending.mp4"
            output = root / "result.mp4"
            payload = b"verified encoded bytes" * 1000
            pending.write_bytes(payload)
            expected = hashlib.sha256(payload).hexdigest()
            with patch.object(os, "link", side_effect=OSError("cross-volume hard links are unavailable")):
                MODULE.publish_output(pending, output, expected)
            self.assertEqual(output.read_bytes(), payload)
            self.assertEqual(pending.read_bytes(), payload)
            with self.assertRaises((OSError, ValueError)):
                MODULE.publish_output(pending, output, expected)
            self.assertEqual(output.read_bytes(), payload)
            failed = root / "failed.mp4"
            with self.assertRaises(ValueError):
                MODULE.publish_output(pending, failed, "0" * 64)
            self.assertFalse(failed.exists())
            with pending.open("r+b") as stream:
                stream.truncate(64 * 1024 * 1024 + 1)
            with self.assertRaises(ValueError):
                MODULE.publish_output(pending, failed, expected)
            self.assertFalse(failed.exists())

    def test_missing_installation_is_refused_without_importing_a_model_runtime(self):
        import sys

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).absolute()
            job = {"mode": "text_to_video", "modelDirectory": str(root / "missing-model"),
                   "runtimeOverlayDirectory": str(root / "missing-overlay")}
            prior = set(sys.modules)
            with self.assertRaises((OSError, ValueError)):
                MODULE.generate(job)
            self.assertNotIn("torch", set(sys.modules) - prior)
            self.assertNotIn("diffusers", set(sys.modules) - prior)

    @unittest.skipUnless(__import__("os").name == "nt", "Windows resource API is required")
    def test_resource_measurement_names_its_actual_process_and_excludes_tree_claim(self):
        import os

        result = MODULE.resource_snapshot()
        self.assertEqual(result["workerPid"], os.getpid())
        self.assertGreater(result["peakWorkerRssMiB"], 0)
        self.assertGreater(result["freeSystemRamBytes"], 0)
        self.assertFalse(result["processTreeMemoryMeasured"])


if __name__ == "__main__":
    unittest.main()
