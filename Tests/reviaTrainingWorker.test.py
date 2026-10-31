import hashlib
import importlib.util
import io
import json
import math
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from unittest import mock


PATH = Path(__file__).resolve().parents[1] / "Tools" / "revia_training_worker.py"
SPEC = importlib.util.spec_from_file_location("revia_training_worker", PATH)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)
SMOKE_MODEL = os.environ.get("REVIA_TRAINING_SMOKE_MODEL", "")
RUNTIME = all(importlib.util.find_spec(name) is not None for name in ("torch", "transformers", "peft"))


def sha(data):
    return hashlib.sha256(data).hexdigest()


class FakeBackend:
    """Deterministic stand-in for the model runtime; it records what the worker asked of it."""

    created = []

    def __init__(self, configuration, device, cancel_at=None, fail_at=None, job_directory=None):
        self.configuration, self.device = configuration, device
        self.cancel_at, self.fail_at, self.job_directory = cancel_at, fail_at, job_directory
        self.steps, self.resumed = 0, None
        FakeBackend.created.append(self)

    def versions(self):
        return {"fixture": "1"}

    def free_vram_mib(self):
        return 8192

    def load(self, directory):
        self.loaded = directory

    def encode(self, messages):
        return messages[-1]["content"]

    def step(self, examples):
        self.steps += 1
        if self.steps == self.fail_at:
            raise RuntimeError("fixture backend failure")
        if self.steps == self.cancel_at:
            (Path(self.job_directory) / "cancel").touch()
        return 1.0 / self.steps

    def evaluate(self, examples):
        return 0.5

    def save_checkpoint(self, directory):
        directory.mkdir(parents=True)
        (directory / "state.json").write_text(json.dumps({"steps": self.steps}))

    def load_checkpoint(self, directory):
        self.steps = json.loads((directory / "state.json").read_text())["steps"]
        self.resumed = directory.name

    def save_adapter(self, directory):
        (directory / "adapter_model.safetensors").write_bytes(b"adapter after %d steps" % self.steps)
        (directory / "adapter_config.json").write_text("{}")


class Fixture:
    def __init__(self, root, base_directory=None):
        self.root = Path(root)
        self.base = Path(base_directory) if base_directory else self.root / "base"
        if not base_directory:
            self.base.mkdir()
            config = {"architectures": ["Qwen3_5ForConditionalGeneration"],
                      "text_config": {"model_type": "qwen3_5_text", "hidden_size": 8, "num_hidden_layers": 2, "vocab_size": 32}}
            (self.base / "config.json").write_text(json.dumps(config))
            (self.base / "chat_template.jinja").write_text("{{ messages }}")
            (self.base / "model.safetensors").write_bytes(b"base weights")
        files = {path.name: {"bytes": path.stat().st_size, "sha256": sha(path.read_bytes())}
                 for path in sorted(self.base.iterdir()) if path.is_file() and not path.name.startswith(".") and
                 path.name != MODULE.INSTALL_MANIFEST}
        install = json.dumps({"repository": "Qwen/Qwen3.5-9B", "revision": "c202236235762e1c871ad0ccb60c8ee5ba337b9a",
                              "files": files}).encode()
        (self.base / MODULE.INSTALL_MANIFEST).write_bytes(install)
        self.dataset = self.root / "dataset"
        self.dataset.mkdir()
        partitions = {"train": [f"t{index}" for index in range(5)], "validation": ["v0"], "test": ["x0"]}
        files, examples = {}, []
        for partition, identifiers in partitions.items():
            lines = ""
            for identifier in identifiers:
                messages = [{"role": "system", "content": "You are Revia."},
                            {"role": "user", "content": f"How did poem {identifier} make you feel?"},
                            {"role": "assistant", "content": f"Honestly, poem {identifier} made me smile."}]
                lines += json.dumps({"id": identifier, "messages": messages}, sort_keys=True) + "\n"
                examples.append({"id": identifier, "eligible": True, "partition": partition, "sources": [f"source-{identifier}"]})
            # Bytes, not text: Windows text mode would turn each newline into CRLF and break the digest.
            (self.dataset / f"{partition}.jsonl").write_bytes(lines.encode("utf-8"))
            files[f"{partition}.jsonl"] = {"sha256": sha(lines.encode()), "examples": len(identifiers)}
        manifest = json.dumps({"schema": "revia-dataset-v1", "name": "fixture", "files": files, "examples": examples}, indent=2).encode()
        (self.dataset / "manifest.json").write_bytes(manifest)
        self.withdrawn = self.root / "withdrawn.json"
        self.withdrawn.write_text("[]")
        self.directory = self.root / "ReviaTrainingJobs" / "job-1"
        self.directory.mkdir(parents=True)
        self.output_root = self.root / "candidates"
        self.output_root.mkdir()
        self.job = {
            "schema": MODULE.SCHEMA, "jobId": "job-1", "scratchRoot": str(self.root / "ReviaTrainingJobs"),
            "outputRoot": str(self.output_root), "withdrawnSourcesPath": str(self.withdrawn),
            "base": {"directory": str(self.base), "repository": "Qwen/Qwen3.5-9B",
                     "revision": "c202236235762e1c871ad0ccb60c8ee5ba337b9a", "installManifestSha256": sha(install)},
            "dataset": {"directory": str(self.dataset), "manifestSha256": sha(manifest)},
            "resources": {"device": "cpu", "minimumFreeVramMiB": 0, "minimumFreeRamMiB": 0, "minimumFreeDiskMiB": 0},
            "configuration": {"method": "sft-lora", "backend": "fixture", "precision": "bf16", "enableThinking": False, "seed": 7,
                              "maxSteps": 6, "learningRate": 0.0002, "gradientAccumulation": 2, "maxSequenceTokens": 2048,
                              "loraRank": 8, "loraAlpha": 16, "loraDropout": 0.0, "targetModules": ["q_proj", "v_proj"],
                              "checkpointEverySteps": 2}}
        self.write_job()

    def write_job(self):
        (self.directory / "job.json").write_text(json.dumps(self.job))

    def run(self, **backend_options):
        factory = lambda configuration, device: FakeBackend(configuration, device, job_directory=self.directory, **backend_options)
        output = io.StringIO()
        with mock.patch.dict(MODULE.BACKENDS, {"fixture": factory}), redirect_stdout(output):
            code = MODULE.main(["--job", str(self.directory / "job.json")])
        events = [json.loads(line) for line in output.getvalue().splitlines()]
        result_path = self.directory / "result.json"
        return code, events, json.loads(result_path.read_text()) if result_path.exists() else None

    @property
    def candidate(self):
        return self.output_root / "adapter-job-1"


class TrainingWorkerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.fixture = Fixture(self.temporary.name)
        FakeBackend.created.clear()

    def tearDown(self):
        self.temporary.cleanup()

    def refused(self, message):
        code, events, result = self.fixture.run()
        self.assertEqual((code, result["state"]), (1, "failed"))
        self.assertIn(message, result["error"])
        self.assertFalse(self.fixture.candidate.exists())
        return result

    def test_complete_job_publishes_an_immutable_bound_candidate(self):
        code, events, result = self.fixture.run()
        self.assertEqual((code, result["state"], result["steps"]), (0, "complete", 6))
        self.assertEqual([event["step"] for event in events if event["state"] == "training"], [1, 2, 3, 4, 5, 6])
        record = json.loads((self.fixture.candidate / "candidate.json").read_text())
        self.assertEqual(result["candidateManifestSha256"], sha((self.fixture.candidate / "candidate.json").read_bytes()))
        self.assertEqual((record["state"], record["base"]["revision"], record["base"]["modelType"]),
                         ("candidate", "c202236235762e1c871ad0ccb60c8ee5ba337b9a", "qwen3_5_text"))
        self.assertEqual(record["dataset"]["manifestSha256"], self.fixture.job["dataset"]["manifestSha256"])
        self.assertEqual((record["dataset"]["trainExamples"], record["dataset"]["validationExamples"]), (5, 1))
        self.assertEqual(record["template"], {"sha256": record["base"]["templateSha256"], "enableThinking": False,
                                              "masking": "assistant reply only"})
        for name, entry in record["files"].items():
            self.assertEqual(entry["sha256"], sha((self.fixture.candidate / name).read_bytes()))
        code, events, _ = self.fixture.run()
        self.assertEqual((code, events[-1]["error"]), (1, "This training job already completed."))

    def test_test_partition_is_never_read(self):
        read = []
        original = MODULE.read_bounded
        with mock.patch.object(MODULE, "read_bounded", lambda path, limit: read.append(Path(path).name) or original(path, limit)):
            code, _, _ = self.fixture.run()
        self.assertEqual(code, 0)
        self.assertIn("train.jsonl", read)
        self.assertNotIn("test.jsonl", read)

    def test_withdrawn_source_refuses_before_any_model_work(self):
        self.fixture.withdrawn.write_text(json.dumps(["source-t3"]))
        self.refused("withdrawn since the export")
        self.assertEqual(FakeBackend.created, [])

    def test_tampered_dataset_or_base_is_refused(self):
        cases = [
            (lambda: (self.fixture.dataset / "train.jsonl").write_text("{}\n"), "does not match its dataset manifest"),
            (lambda: self.fixture.job["dataset"].update(manifestSha256="0" * 64), "dataset manifest does not match"),
            (lambda: self.fixture.job["base"].update(installManifestSha256="0" * 64), "install manifest does not match"),
            (lambda: self.fixture.job["base"].update(revision="1" * 40), "different repository or revision"),
            (lambda: (self.fixture.base / "chat_template.jinja").write_text("{{ messagez }}"), "changed since installation"),
            (lambda: (self.fixture.base / "model.safetensors").write_bytes(b"short"), "wrong size"),
        ]
        for change, message in cases:
            with self.subTest(message=message):
                self.temporary.cleanup()
                self.temporary = tempfile.TemporaryDirectory()
                self.fixture = Fixture(self.temporary.name)
                change()
                self.fixture.write_job()
                self.refused(message)

    def test_unqualified_configurations_are_refused(self):
        cases = [({"precision": "qlora"}, "Qwen3.5 guidance recommends BF16 LoRA"), ({"precision": "fp16"}, "Only BF16"),
                 ({"enableThinking": True}, "non-reasoning"), ({"backend": "remote"}, "Unknown training backend"),
                 ({"maxSteps": 0}, "maxSteps"), ({"targetModules": []}, "targetModules"),
                 ({"checkpointEverySteps": 9}, "checkpointEverySteps"), ({"learningRate": 0.5}, "learningRate")]
        original = dict(self.fixture.job["configuration"])
        for change, message in cases:
            with self.subTest(change=change):
                self.fixture.job["configuration"] = {**original, **change}
                self.fixture.write_job()
                self.refused(message)
        self.fixture.job["configuration"] = original
        self.fixture.job["resources"]["device"] = "cuda:9"
        self.fixture.write_job()
        self.refused("device")

    def test_cancel_keeps_a_checkpoint_and_resume_continues_the_same_order(self):
        code, events, result = self.fixture.run(cancel_at=3)
        self.assertEqual((code, result["state"], result["step"]), (3, "cancelled", 3))
        self.assertFalse(self.fixture.candidate.exists())
        (self.fixture.directory / "cancel").unlink()
        code, events, result = self.fixture.run()
        self.assertEqual((code, result["state"], result["resumedFromStep"]), (0, "complete", 3))
        self.assertEqual(FakeBackend.created[-1].resumed, "checkpoint-000003")
        self.assertEqual([event["step"] for event in events if event["state"] == "training"], [4, 5, 6])
        self.assertEqual(json.loads((self.fixture.directory / "result-1.json").read_text())["state"], "cancelled")

    def test_resume_refuses_another_configuration(self):
        self.fixture.run(cancel_at=2)
        (self.fixture.directory / "cancel").unlink()
        self.fixture.job["configuration"]["learningRate"] = 0.0001
        self.fixture.write_job()
        self.refused("different base, dataset or configuration")

    def test_ram_floor_and_backend_failure_publish_nothing(self):
        readings = iter([4096, 4096, 10])
        with mock.patch.object(MODULE, "free_ram_mib", lambda: next(readings, 10)):
            self.fixture.job["resources"]["minimumFreeRamMiB"] = 1024
            self.fixture.write_job()
            self.refused("below the job's floor during training")
        self.temporary.cleanup()
        self.temporary = tempfile.TemporaryDirectory()
        self.fixture = Fixture(self.temporary.name)
        code, _, result = self.fixture.run(fail_at=2)
        self.assertEqual((code, result["error"]), (1, "fixture backend failure"))
        self.assertEqual(list(self.fixture.output_root.iterdir()), [])

    def test_existing_candidate_is_never_replaced(self):
        self.fixture.candidate.mkdir()
        code, _, result = self.fixture.run()
        self.assertEqual((code, result["state"]), (1, "failed"))
        self.assertIn("never replaced", result["error"])
        self.assertEqual(list(self.fixture.candidate.iterdir()), [])

    def test_schedule_covers_each_epoch_and_depends_only_on_step(self):
        picks = [MODULE.schedule(5, 7, step, 2) for step in range(10)]
        flat = [index for step in picks for index in step]
        for epoch in range(4):
            self.assertEqual(sorted(flat[epoch * 5:(epoch + 1) * 5]), list(range(5)))
        self.assertEqual(picks, [MODULE.schedule(5, 7, step, 2) for step in range(10)])

    def test_missing_runtime_is_refused_without_importing_it(self):
        with mock.patch.dict(sys.modules, {"peft": None}):
            with self.assertRaisesRegex(ValueError, "optional training runtime is not installed: peft"):
                MODULE.TransformersPeftBackend({}, "cpu")

    @unittest.skipUnless(SMOKE_MODEL and Path(SMOKE_MODEL).is_dir() and RUNTIME,
                         "Needs the training runtime and REVIA_TRAINING_SMOKE_MODEL set to a local tiny model")
    def test_real_backend_trains_a_tiny_local_model(self):
        import torch
        from peft import PeftModel
        from transformers import AutoModelForCausalLM, AutoTokenizer

        smoke = Path(self.temporary.name) / "smoke"
        shutil.copytree(SMOKE_MODEL, smoke / "base", ignore=shutil.ignore_patterns(".*"))
        fixture = Fixture(smoke, base_directory=smoke / "base")
        fixture.job["configuration"].update(backend="transformers-peft", maxSteps=4, gradientAccumulation=1,
                                            checkpointEverySteps=2, learningRate=0.01, loraRank=4, loraAlpha=8,
                                            targetModules=["q_proj", "k_proj", "v_proj", "o_proj"])
        fixture.write_job()
        original = MODULE.emit

        def cancel_after_second_step(state, **values):
            original(state, **values)
            if state == "training" and values.get("step") == 2:
                (fixture.directory / "cancel").touch()

        # The first attempt is cancelled at its step-2 checkpoint; the second reloads it.
        with mock.patch.object(MODULE, "emit", cancel_after_second_step), redirect_stdout(io.StringIO()):
            self.assertEqual(MODULE.main(["--job", str(fixture.directory / "job.json")]), 3)
        (fixture.directory / "cancel").unlink()
        with redirect_stdout(io.StringIO()):
            code = MODULE.main(["--job", str(fixture.directory / "job.json")])
        result = json.loads((fixture.directory / "result.json").read_text())
        self.assertEqual((code, result["state"], result["resumedFromStep"]), (0, "complete", 2), result)
        self.assertTrue(math.isfinite(result["validationLoss"]))
        record = json.loads((fixture.candidate / "candidate.json").read_text())
        self.assertIn("adapter_model.safetensors", record["files"])
        tokenizer = AutoTokenizer.from_pretrained(smoke / "base", local_files_only=True)
        tokens = torch.tensor([tokenizer("Honestly, poem t1 made me smile.")["input_ids"]])
        base = AutoModelForCausalLM.from_pretrained(smoke / "base", local_files_only=True, dtype=torch.bfloat16).eval()
        with torch.no_grad():
            before = base(input_ids=tokens).logits
            adapted = PeftModel.from_pretrained(base, str(fixture.candidate)).eval()
            after = adapted(input_ids=tokens).logits
        self.assertFalse(torch.equal(before, after), "The published adapter did not change the base model's output.")


if __name__ == "__main__":
    unittest.main(verbosity=2)
