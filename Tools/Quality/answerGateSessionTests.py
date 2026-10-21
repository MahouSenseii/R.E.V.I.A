"""Local fixtures only: no model, server executable, or external service."""
import argparse
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import answerGateSession as session


class SessionPlanTests(unittest.TestCase):
    def plan(self, **changes):
        values = dict(gate=Path("gate.exe"), profile=Path("profile.json"), corpus=Path("cases.jsonl"),
                      output=Path("results"), context=8192, max_tokens=512, seeds=[0, 1],
                      timeout_seconds=10800, experiment="control", category=[], record_baseline=False,
                      baseline=None, commit="test", no_judge=False, cache_type_k="f16", cache_type_v="f16", device="CUDA0")
        values.update(changes)
        return argparse.Namespace(**values)

    def test_canonical_baseline_requires_control_and_repeated_full_corpus(self):
        for changes in (dict(category=["math"]), dict(experiment="omit-state-packet"), dict(seeds=[0]), dict(no_judge=True)):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                session.validate_plan(self.plan(record_baseline=True, **changes))

    def test_workload_is_finite_and_seeds_are_distinct(self):
        for changes in (dict(seeds=[0, 0]), dict(seeds=list(range(5))), dict(timeout_seconds=0),
                        dict(timeout_seconds=28801), dict(max_tokens=8192)):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                session.validate_plan(self.plan(**changes))

    def test_gate_arguments_bind_endpoint_without_plaintext_secret(self):
        args = self.plan(record_baseline=True)
        command = session.gate_arguments(args, 12345, "owned-alias", 0, Path("seed-0"))
        self.assertIn("--expected-model", command)
        self.assertIn("owned-alias", command)
        self.assertIn("--api-key-env", command)
        self.assertIn(session.KEY_ENV, command)
        self.assertIn("--ignore-baseline", command)
        self.assertEqual(command[command.index("--max-unavailable") + 1], "0")
        self.assertEqual(command[command.index("--min-judge-coverage") + 1], "100")
        self.assertIn("--write-baseline", command)
        self.assertNotIn("--write-baseline", session.gate_arguments(args, 12345, "owned-alias", 1, Path("seed-1")))

    def test_redaction_handles_chunk_boundaries(self):
        class Chunks:
            def __init__(self): self.parts = iter([b"first abc", b"defgh last abcdefgh", b""])
            def read(self, _): return next(self.parts)
        output = io.BytesIO()
        session.copy_redacted_stream(Chunks(), output, b"abcdefgh")
        self.assertEqual(output.getvalue(), b"first [REDACTED] last [REDACTED]")

    def run_fake(self, fail=False, no_judge=True):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            paths = {}
            for name in ("server", "model", "gate", "profile", "corpus"):
                path = root / name
                path.write_text("public fixture", encoding="utf-8")
                paths[name] = path
            args = self.plan(**paths, output=root / "results", no_judge=no_judge)
            children = []
            keys = []

            class Process:
                pid = 123
                returncode = None
                def wait(self, timeout):
                    if fail:
                        raise subprocess.TimeoutExpired("fixture-gate", timeout)
                    self.returncode = 0
                    return 0
                def poll(self): return self.returncode

            class Child:
                def __init__(self, arguments, directory, environment, log_path, key):
                    self.process, self.closed = Process(), False
                    keys.append(key)
                    self.arguments = arguments
                    children.append(self)
                    if arguments[0] == str(args.gate):
                        self.assertions = (environment[session.KEY_ENV] == key and key not in " ".join(arguments))
                        directory = Path(arguments[arguments.index("--output-dir") + 1])
                        directory.mkdir()
                        (directory / "fake.summary.json").write_text(json.dumps({"gate": {"outcome": "no_baseline"},
                            "summary": {"overall": {"cases": 1, "judged": 0, "unavailable": 0}}}))
                def __enter__(self): return self
                def __exit__(self, *_):
                    self.process.returncode = -15 if self.process.returncode is None else self.process.returncode
                    self.closed = True

            with patch.object(session, "OwnedChild", Child), \
                 patch.object(session.backend, "require_api_key_file_support"), \
                 patch.object(session.backend, "fresh_port", return_value=12345), \
                 patch.object(session.backend, "wait_ready", return_value={"default_generation_settings": {"n_ctx": 8192}, "total_slots": 1}):
                code = session.run_session(args)
            report = json.loads((args.output / "session.json").read_text())
            self.assertTrue(all(child.closed for child in children))
            self.assertTrue(all(getattr(child, "assertions", True) for child in children))
            self.assertTrue(report["ownedServerStopped"])
            for artifact in args.output.rglob("*"):
                if artifact.is_file():
                    self.assertTrue(all(key.encode("ascii") not in artifact.read_bytes() for key in keys))
            key_path = Path(report["serverArguments"][report["serverArguments"].index("--api-key-file") + 1])
            self.assertFalse(key_path.exists())
            return code, report

    def test_full_mock_campaign_keeps_owned_auth_and_cleans_children(self):
        code, report = self.run_fake()
        self.assertEqual(code, 0)
        self.assertEqual(report["status"], "completed")
        self.assertEqual([row["seed"] for row in report["runs"]], [0, 1])

    def test_timeout_keeps_evidence_and_cleans_owned_processes(self):
        code, report = self.run_fake(fail=True)
        self.assertEqual(code, 2)
        self.assertEqual(report["status"], "incomplete")

    def test_missing_judgments_cannot_complete_even_if_gate_returns_zero(self):
        code, report = self.run_fake(no_judge=False)
        self.assertEqual(code, 2)
        self.assertEqual(report["runs"][0]["coverage"]["judged"], 0)
        self.assertEqual(report["status"], "incomplete")

    def test_owned_child_captures_redacted_log_and_stops_exact_process(self):
        class Process:
            stdout = io.BytesIO(b"header owned-secret trailer")
            returncode = None
        process = Process()
        with tempfile.TemporaryDirectory() as temporary, \
             patch.object(session.subprocess, "Popen", return_value=process) as popen, \
             patch.object(session.backend, "stop_owned_process") as stop:
            log = Path(temporary) / "owned.log"
            with session.OwnedChild(["fixture"], temporary, {}, log, "owned-secret"):
                pass
            stop.assert_called_once_with(process)
            self.assertEqual(log.read_bytes(), b"header [REDACTED] trailer")
            self.assertEqual(popen.call_args.kwargs["stdin"], subprocess.DEVNULL)


if __name__ == "__main__":
    unittest.main()
