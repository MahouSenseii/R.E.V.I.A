import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import struct
import sys
import tempfile
import unittest
from contextlib import redirect_stdout


ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


EXPORT = load("revia_adapter_export", ROOT / "Tools" / "revia_adapter_export.py")
WORKER_TESTS = load("revia_training_worker_tests", ROOT / "Tests" / "reviaTrainingWorker.test.py")
SMOKE = {name: os.environ.get(name, "") for name in
         ("REVIA_TRAINING_SMOKE_MODEL", "REVIA_LLAMA_CPP_DIR", "REVIA_LLAMA_BIN_DIR", "REVIA_CONVERTER_PYTHON")}
RUNTIME = all(importlib.util.find_spec(name) is not None for name in ("torch", "transformers", "peft"))


def sha(data):
    return hashlib.sha256(data).hexdigest()


def gguf(entries):
    """A header-only GGUF v3 file with the given string, uint32 and string-array values."""
    def text(value):
        data = value.encode()
        return struct.pack("<Q", len(data)) + data
    body = b""
    for key, value in entries.items():
        body += text(key)
        if isinstance(value, str):
            body += struct.pack("<I", 8) + text(value)
        elif isinstance(value, list):
            body += struct.pack("<IIQ", 9, 8, len(value)) + b"".join(text(item) for item in value)
        else:
            body += struct.pack("<II", 4, value)
    return b"GGUF" + struct.pack("<IQQ", 3, 0, len(entries)) + body


class AdapterExportTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.fixture = WORKER_TESTS.Fixture(self.root)
        job = self.fixture.job["base"]
        self.base = EXPORT.WORKER.verify_base({"base": job})

    def tearDown(self):
        self.temporary.cleanup()

    def candidate(self, base):
        directory = self.root / "candidate"
        directory.mkdir()
        (directory / "adapter_model.safetensors").write_bytes(b"adapter")
        record = {"schema": EXPORT.WORKER.CANDIDATE_SCHEMA, "state": "candidate", "jobId": "job-1", "base": base,
                  "configurationSha256": "0" * 64, "dataset": {},
                  "files": {"adapter_model.safetensors": {"sha256": sha(b"adapter"), "bytes": 7}}}
        (directory / "candidate.json").write_text(json.dumps(record))
        return directory

    def test_candidate_for_another_base_is_refused(self):
        other = {**self.base, "revision": "1" * 40}
        with self.assertRaisesRegex(ValueError, "different base checkpoint"):
            EXPORT.verify_candidate(self.candidate(other), self.base)

    def test_changed_candidate_file_is_refused(self):
        directory = self.candidate(self.base)
        self.assertEqual(EXPORT.verify_candidate(directory, self.base)["jobId"], "job-1")
        (directory / "adapter_model.safetensors").write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "changed after the job published it"):
            EXPORT.verify_candidate(directory, self.base)

    def test_outputs_are_never_replaced(self):
        (self.root / "taken").mkdir()
        with self.assertRaisesRegex(ValueError, "never replaced"):
            EXPORT.new_directory(self.root / "taken")
        (self.root / ".unfinished.partial").mkdir()
        with self.assertRaisesRegex(ValueError, "never replaced"):
            EXPORT.new_directory(self.root / "unfinished")

    def test_gguf_identity_reads_the_header_only(self):
        path = self.root / "model.gguf"
        path.write_bytes(gguf({"general.architecture": "qwen35", "qwen35.block_count": 2, "general.file_type": 15,
                               "tokenizer.ggml.tokens": ["a", "b"], "tokenizer.chat_template": "T"}))
        identity = EXPORT.gguf_identity(path)
        self.assertEqual((identity["architecture"], identity["blockCount"], identity["fileType"], identity["tensors"]),
                         ("qwen35", 2, 15, 0))
        self.assertEqual(identity["chatTemplateSha256"], sha(b"T"))
        path.write_bytes(b"NOPE")
        with self.assertRaisesRegex(ValueError, "not a GGUF file"):
            EXPORT.gguf_identity(path)

    def test_failed_step_reports_its_own_label(self):
        with self.assertRaisesRegex(ValueError, "Base conversion failed with exit 3: boom"):
            EXPORT.run("Base conversion", [sys.executable, "-c", "import sys; sys.stderr.write('boom'); sys.exit(3)"], 60)

    def test_check_refuses_an_adapter_export_for_another_base(self):
        exports = {}
        for kind, base in (("base", self.base), ("adapter", {**self.base, "revision": "1" * 40})):
            directory = self.root / f"{kind}-export"
            directory.mkdir()
            (directory / "file.gguf").write_bytes(kind.encode())
            record = {"kind": kind, "base": base, "files": {"file.gguf": {"sha256": sha(kind.encode()), "bytes": len(kind)}}}
            (directory / "export.json").write_text(json.dumps(record))
            exports[kind] = directory
        arguments = type("Arguments", (), {"base_export": exports["base"], "adapter_export": exports["adapter"], "base": self.fixture.base})
        with self.assertRaisesRegex(ValueError, "different base export"):
            EXPORT.command_check(arguments)

    @unittest.skipUnless(all(SMOKE.values()) and RUNTIME,
                         "Needs the training runtime and the tiny model, llama.cpp source, binaries and converter Python")
    def test_tiny_model_roundtrip_through_llama_cpp(self):
        smoke = self.root / "smoke"
        shutil.copytree(SMOKE["REVIA_TRAINING_SMOKE_MODEL"], smoke / "base", ignore=shutil.ignore_patterns(".*"))
        # The official Qwen3.5 checkpoints name Qwen2Tokenizer. The tiny model was saved by transformers v5 as
        # TokenizersBackend, which the converter's pinned transformers 4.57.6 cannot load; match the real bases.
        tokenizer_config = smoke / "base" / "tokenizer_config.json"
        settings = json.loads(tokenizer_config.read_text())
        if settings.get("tokenizer_class") == "TokenizersBackend":
            settings["tokenizer_class"] = "Qwen2Tokenizer"
            tokenizer_config.write_text(json.dumps(settings, indent=2))
        fixture = WORKER_TESTS.Fixture(smoke, base_directory=smoke / "base")
        fixture.job["configuration"].update(backend="transformers-peft", maxSteps=4, gradientAccumulation=1, learningRate=0.01,
                                            loraRank=4, loraAlpha=8,
                                            # The 4B experiment's set: attention in the full-attention layers, MLP in every layer.
                                            targetModules=["q_proj", "k_proj", "v_proj", "o_proj", "gate_proj", "up_proj", "down_proj"])
        fixture.write_job()
        with redirect_stdout(io.StringIO()):
            self.assertEqual(EXPORT.WORKER.main(["--job", str(fixture.directory / "job.json")]), 0)
        base_job = fixture.job["base"]
        common = ["--base", base_job["directory"], "--repository", base_job["repository"], "--revision", base_job["revision"],
                  "--install-manifest-sha256", base_job["installManifestSha256"], "--llama-cpp", SMOKE["REVIA_LLAMA_CPP_DIR"],
                  "--converter-python", SMOKE["REVIA_CONVERTER_PYTHON"]]
        binaries = Path(SMOKE["REVIA_LLAMA_BIN_DIR"])
        suffix = ".exe" if os.name == "nt" else ""
        with redirect_stdout(io.StringIO()):
            # The tiny model's 16-wide tensors cannot be block-quantized (Q8_0 needs 32, K-quants 256), so the step
            # requantizes to F16; the real 4B and 9B bases use Q4_K_M.
            self.assertEqual(EXPORT.main(["base", *common, "--output", str(smoke / "base-export"),
                                          "--quantize", str(binaries / f"llama-quantize{suffix}"), "--quantization", "F16"]), 0)
            self.assertEqual(EXPORT.main(["adapter", *common, "--output", str(smoke / "adapter-export"),
                                          "--candidate", str(fixture.candidate)]), 0)
            code = EXPORT.main(["check", "--base", base_job["directory"], "--base-export", str(smoke / "base-export"),
                                "--adapter-export", str(smoke / "adapter-export"), "--server", str(binaries / f"llama-server{suffix}"),
                                "--output", str(smoke / "check.json")])
        record = json.loads((smoke / "check.json").read_text())
        self.assertEqual((code, record["passed"]), (0, True), json.dumps(record, indent=1))
        for run in ("base", "adapter"):
            self.assertEqual(record["runs"][run]["checks"], {"template": True, "endOfTurn": True, "rendering": True, "tokenizer": True})
        # The identity and the adapter flag are what later steps bind to; a run's results must not overwrite them.
        base_export = json.loads((smoke / "base-export" / "export.json").read_text())
        self.assertEqual((record["base"], record["adapter"]), (base_export["base"], True))


if __name__ == "__main__":
    unittest.main(verbosity=2)
