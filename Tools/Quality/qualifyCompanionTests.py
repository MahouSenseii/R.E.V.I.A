"""Behavioral checks for the finite qualification protocol."""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import uuid
from contextlib import contextmanager

sys.dont_write_bytecode = True
import qualify_companion as qualification

WORKER = None
EVIDENCE = None
SECRET = "qual1-synthetic-credential-" + uuid.uuid4().hex


@contextmanager
def owned_directory():
    parent = Path(__file__).resolve().parents[3] / "qualification/protocol-artifacts"
    parent.mkdir(parents=True, exist_ok=True)
    directory = parent / uuid.uuid4().hex
    directory.mkdir()
    yield str(directory)  # Retained failure evidence; no directory deletion or chmod.


class OracleTests(unittest.TestCase):
    @staticmethod
    def complete_pair():
        relationship = {"entity_id": "participant:fixture", "display_name": "QualificationOwner", "first_seen": "now", "interactions": 2}
        checks = {name: "passed" for name in ("no_child_processes_allowed", "optional_hosts_disabled_before_start", "memory_enabled_no_archive_tail", "production_stop")}
        write = {"status": "passed", "process_exit": 0, "pid": 1, "revision_count": 1, "companion_id": "qualification",
            "relationship": relationship, "checks": {**checks, "initial_automatic_save": "passed", "automatic_proposal": "passed", "durable_acceptance": "passed"},
            "durable_receipt": {"request_id": "request-1", "digest": "digest-1", "original_id": "old", "revised_id": "new", "subject_id": "participant:fixture"}}
        recall = {"status": "passed", "process_exit": 0, "pid": 2, "revision_count": 1, "companion_id": "qualification",
            "relationship_loaded": relationship, "relationship": relationship, "checks": {**checks, "public_private_rows_unchanged": "passed", "public_private_revisions_unchanged": "passed"},
            "retrieved_active": ["- [project] [subject: participant participant:fixture] [source: automatic; id: new] [current owner revision; receipt: request-1; current id: new] The user uses Maple as Project Juniper's label."],
            "answer": "Maple", "private_control_answer": "Maple", "routing_passed": True, "working_context_before_introduction": 0}
        return write, recall

    def test_complete_protocol_pair_is_admissible(self):
        self.assertTrue(qualification.positive_oracle(*self.complete_pair()))

    def test_wrong_retrieved_receipt_cannot_borrow_durable_acceptance(self):
        write, recall = self.complete_pair()
        recall["retrieved_active"][0] = recall["retrieved_active"][0].replace("request-1", "foreign-receipt")
        self.assertFalse(qualification.positive_oracle(write, recall))

    def test_wrong_retrieved_row_cannot_borrow_durable_acceptance(self):
        write, recall = self.complete_pair()
        recall["retrieved_active"][0] = recall["retrieved_active"][0].replace("id: new", "id: foreign")
        self.assertFalse(qualification.positive_oracle(write, recall))

    def test_missing_owner_checks_cannot_pass(self):
        write, recall = self.complete_pair()
        write["checks"] = {}
        self.assertFalse(qualification.positive_oracle(write, recall))

    def test_restored_history_cannot_pass(self):
        write, recall = self.complete_pair()
        recall["working_context_before_introduction"] = 2
        self.assertFalse(qualification.positive_oracle(write, recall))

    def test_reintroduced_foreign_identity_cannot_pass(self):
        write, recall = self.complete_pair()
        recall["relationship"] = {"entity_id": "foreign", "display_name": "QualificationOwner"}
        self.assertFalse(qualification.positive_oracle(write, recall))

    def test_plausible_answer_without_durable_receipt_cannot_pass(self):
        self.assertFalse(qualification.positive_oracle(
            {"status": "failed", "durable_receipt": None},
            {"status": "passed", "answer": "Maple", "retrieved_active": ["Maple"]}))

    def test_answer_without_admitted_retrieval_cannot_pass(self):
        self.assertFalse(qualification.positive_oracle(
            {"status": "passed", "durable_receipt": {"request_id": "r"}},
            {"status": "passed", "answer": "Maple", "retrieved_active": []}))

    def test_provider_answer_depends_on_current_admitted_record(self):
        provider = qualification.SyntheticProvider(0, SECRET)
        try:
            self.assertNotIn("Maple", provider.answer({"messages": [{"content": "The answer is Maple."}]}))
            reference = qualification.MEMORY_MARKER + "\n- [project] [subject: participant p] [current owner revision; receipt: r] The user uses Maple as Project Juniper's label."
            body = {"messages": [{"role": "user", "content": reference}]}
            self.assertIn("Maple", provider.answer(body))
            body["messages"][0]["content"] += "\n- [project] [subject: participant p] The user uses Cedar as Project Juniper's label."
            self.assertNotIn("Maple", provider.answer(body))
        finally:
            provider.server.server_close()

    def test_provider_declines_unknown_speaker_extraction(self):
        provider = qualification.SyntheticProvider(0, SECRET)
        try:
            body = {"response_format": {"type": "json_object"}, "messages": [{"content": json.dumps({"user_message": qualification.INITIAL, "captured_speaker_known": False})}]}
            self.assertFalse(json.loads(provider.answer(body))["shouldRemember"])
        finally:
            provider.server.server_close()

    def test_superseded_audit_history_does_not_hide_current_revision(self):
        provider = qualification.SyntheticProvider(0, SECRET)
        try:
            write, recall = self.complete_pair()
            history = "- [project] [subject: participant participant:fixture] [source: automatic; id: old] [historical owner-revised record; receipt: request-1; current id: new] The user uses Cedar as Project Juniper's label."
            body = {"messages": [{"role": "user", "content": qualification.MEMORY_MARKER + "\n" + history + "\n" + recall["retrieved_active"][0]}]}
            self.assertIn("Maple", provider.answer(body))
        finally:
            provider.server.server_close()


class OwnershipTests(unittest.TestCase):
    def test_long_runtime_path_is_blocked_before_any_mutation(self):
        with owned_directory() as folder:
            root = Path(folder)
            runtime = root / ("r" * 120)
            with self.assertRaises(qualification.Blocked):
                qualification.fresh_paths(runtime, root / "report.json")
            self.assertFalse(runtime.exists())
    def test_unsupported_live_mode_is_blocked_with_fresh_failure_report(self):
        with owned_directory() as folder:
            root = Path(folder)
            args = qualification.parser().parse_args(["--scenario", "memory-restart", "--runtime-root", str(root / "runtime"),
                "--provider-port", "0", "--api-key-env", "QUAL1_TEST_KEY", "--report", str(root / "report.json"),
                "--worker", sys.executable])
            result = qualification.run(args)
            self.assertEqual(result["status"], "blocked")
            self.assertEqual(json.loads((root / "report.json").read_text())["real_model"], "not_run")
            self.assertFalse((root / "runtime").exists())

    def test_nonempty_root_is_preserved(self):
        with owned_directory() as folder:
            root = Path(folder)
            runtime = root / "runtime"
            runtime.mkdir()
            unrelated = runtime / "unrelated.txt"
            unrelated.write_text("preserve me")
            with self.assertRaises(qualification.Blocked):
                qualification.fresh_paths(runtime, root / "report.json")
            self.assertEqual(unrelated.read_text(), "preserve me")

    def test_reused_report_is_preserved(self):
        with owned_directory() as folder:
            root = Path(folder)
            report = root / "report.json"
            report.write_text("prior receipt")
            with self.assertRaises(qualification.Blocked):
                qualification.fresh_paths(root / "runtime", report)
            self.assertEqual(report.read_text(), "prior receipt")

    def test_report_inside_runtime_refused(self):
        with owned_directory() as folder:
            root = Path(folder)
            with self.assertRaises(qualification.Blocked):
                qualification.fresh_paths(root, root / "report.json")

    def test_redirected_root_is_refused_and_target_preserved(self):
        with owned_directory() as folder:
            root = Path(folder)
            target, link = root / "target", root / "link"
            target.mkdir()
            (target / "keep").write_text("unrelated")
            if os.environ.get("QUAL1_JUNCTION_BLOCKED_EVIDENCE"):
                self.fail("BLOCKED: actual redirected-path fixture unavailable; " + os.environ["QUAL1_JUNCTION_BLOCKED_EVIDENCE"])
            if os.name == "nt":
                command = ["cmd", "/c", "mklink", "/J", str(link), str(target)]
                outcome = subprocess.run(command, capture_output=True)
                (root / "junction-attempt.json").write_text(json.dumps({"command": command, "exit_code": outcome.returncode,
                    "stdout": outcome.stdout.decode(errors="replace"), "stderr": outcome.stderr.decode(errors="replace")}, indent=2))
                self.assertEqual(outcome.returncode, 0, "BLOCKED: Could not create owned junction fixture; " + str(root / "junction-attempt.json"))
            else:
                link.symlink_to(target, target_is_directory=True)
            try:
                with self.assertRaises(qualification.Blocked):
                    qualification.fresh_paths(link, root / "report.json")
                self.assertEqual((target / "keep").read_text(), "unrelated")
            finally:
                pass  # Retain only our junction and target as diagnostic artifacts.

    def test_occupied_port_is_not_attached_or_closed(self):
        listener = socket.socket()
        listener.bind(("127.0.0.1", 0))
        listener.listen()
        try:
            with self.assertRaises(OSError):
                qualification.SyntheticProvider(listener.getsockname()[1], SECRET)
            self.assertGreater(listener.fileno(), 0)
        finally:
            listener.close()

    def test_recursive_redaction_including_keys(self):
        self.assertNotIn(SECRET, json.dumps(qualification.redact({SECRET: [SECRET, "Bearer " + SECRET]}, SECRET)))

    def test_duplicate_json_is_refused(self):
        with self.assertRaises(ValueError):
            qualification.unique_json('{"phase":"write-correct","phase":"recall"}')

    def test_public_fields_have_separate_sentinel_checks(self):
        checks = qualification.routing_checks([{"stage": "recall/public", "body": {"model": "x", "messages": [{"content": "Maple"}]}}])
        self.assertEqual(checks["public[0].body.messages[0].content.excludes_Maple"], "failed")
        self.assertEqual(checks["public[0].body.messages[0].content.excludes_Cedar"], "passed")

    def child(self, mode, timeout=3, inspect_receipt=False):
        with owned_directory() as folder:
            root = Path(folder)
            receipt = root / "phase.json"
            env = os.environ.copy()
            env["QUAL1_TEST_KEY"] = SECRET
            command = [sys.executable, str(Path(__file__).resolve()), "--fake-child", mode, str(receipt), str(root)]
            value = qualification.run_child(command, receipt, "write-correct", "test-run", root, "none", env, SECRET, timeout)
            self.assertNotIn(SECRET, json.dumps(value))
            if inspect_receipt:
                raw = receipt.read_bytes()
                retained = SECRET.encode() in raw
                # RED safety cleanup is independent of the assertion and is not runner success.
                if retained:
                    receipt.write_bytes(raw.replace(SECRET.encode(), b"[TEST_SAFETY_REDACTED]"))
                value["test_observed_raw_receipt_secret"] = retained
            return value

    def test_child_cannot_overwrite_parent_observations(self):
        result = self.child("overwrite-metadata")
        self.assertEqual(result["status"], "failed")
        self.assertTrue(result["credential_in_child_output"])
        self.assertEqual(result["cleanup"], "exited")
        self.assertEqual(result["process_exit"], 0)
        self.assertGreaterEqual(result["seconds"], 0)
        self.assertIn("[REDACTED]", result["stdout"])

    def test_invalid_receipts_are_sanitized_before_validation(self):
        for mode in ("malformed-secret", "wrong-phase-secret", "duplicate-secret", "invalid-utf8-secret"):
            with self.subTest(mode=mode):
                result = self.child(mode, inspect_receipt=True)
                self.assertFalse(result["test_observed_raw_receipt_secret"], "Runner retained raw credential bytes in rejected receipt")
                self.assertEqual(result["status"], "failed")
                self.assertTrue(result["credential_in_phase_receipt"])

    def test_incomplete_owned_requests_cannot_block_cleanup(self):
        for mode in ("partial-line", "partial-body"):
            with self.subTest(mode=mode):
                env = os.environ.copy()
                env["QUAL1_TEST_KEY"] = SECRET
                command = [sys.executable, "-B", str(Path(__file__).resolve()), "--fake-provider", mode]
                child = subprocess.Popen(command, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                    creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
                finished = True
                try:
                    stdout, stderr = child.communicate(timeout=4)
                except subprocess.TimeoutExpired:
                    finished = False
                    child.kill()
                    stdout, stderr = child.communicate(timeout=2)
                self.assertTrue(finished, "Provider exit exceeded hard bound; sole owned fixture child killed/reaped")
                self.assertEqual(child.returncode, 0, qualification.redact(stderr.decode(errors="replace"), SECRET))
                result = json.loads(stdout)
                self.assertEqual(result["cleanup"], "closed")
                self.assertFalse(result["listener_alive"])
                self.assertLess(result["seconds"], 3)

    def test_missing_child_receipt_fails(self):
        self.assertEqual(self.child("missing")["status"], "failed")

    def test_malformed_child_receipt_fails(self):
        self.assertEqual(self.child("malformed")["status"], "failed")

    def test_wrong_phase_receipt_fails(self):
        self.assertEqual(self.child("wrong-phase")["status"], "failed")

    def test_nonzero_exit_cannot_borrow_passed_receipt(self):
        self.assertEqual(self.child("bad-exit")["status"], "failed")

    def test_credential_in_child_output_is_redacted_failure(self):
        result = self.child("secret")
        self.assertEqual(result["status"], "failed")
        self.assertTrue(result["credential_in_child_output"])

    def test_timeout_kills_only_owned_child_and_is_blocked(self):
        result = self.child("timeout", .15)
        self.assertEqual(result["status"], "blocked")
        self.assertEqual(result["cleanup"], "terminated_owned_process")
        self.assertIsNotNone(result["process_exit"])


class NativeTests(unittest.TestCase):
    def run_fixture(self, fault):
        directory = EVIDENCE / (fault + "-" + uuid.uuid4().hex[:8])
        directory.mkdir(parents=True)
        args = qualification.parser().parse_args(["--scenario", "memory-restart", "--runtime-root", str(directory / "runtime"),
            "--provider-port", "0", "--api-key-env", "QUAL1_TEST_KEY", "--report", str(directory / "report.json"),
            "--fixture", "--worker", str(WORKER), "--fault", fault])
        os.environ["QUAL1_TEST_KEY"] = SECRET
        try:
            result = qualification.run(args)
        finally:
            del os.environ["QUAL1_TEST_KEY"]
        print(json.dumps({"fault": fault, "status": result["status"], "report": str(directory / "report.json")}), flush=True)
        for path in directory.rglob("*"):
            if path.is_file():
                self.assertNotIn(SECRET.encode(), path.read_bytes(), str(path))
        self.assertEqual(result["real_model"], "not_run")
        return result

    def test_two_native_processes_recall_and_route(self):
        result = self.run_fixture("none")
        self.assertEqual(result["status"], "passed", json.dumps(result.get("phases"), indent=2))
        self.assertTrue(result["positive_restart_oracle"])
        self.assertEqual(result["provider_cleanup"], "closed")
        self.assertNotEqual(result["phases"][0]["pid"], result["phases"][1]["pid"])

    def test_negative_native_controls_cannot_pass_restart_oracle(self):
        for fault in qualification.FAULTS[1:]:
            with self.subTest(fault=fault):
                result = self.run_fixture(fault)
                self.assertEqual(result["status"], "failed")
                self.assertFalse(result["positive_restart_oracle"])
                if fault == "interrupt-before-receipt":
                    self.assertEqual(result["phases"][0]["status"], "interrupted")
                    self.assertEqual(result["phases"][1]["status"], "not_run")
                else:
                    self.assertEqual(len(result["phases"]), 2)
                    self.assertTrue(all(x["process_exit"] == 0 for x in result["phases"]))


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--fake-provider":
        mode = sys.argv[2]
        accepted = threading.Event()
        secret = os.environ["QUAL1_TEST_KEY"]
        provider = qualification.SyntheticProvider(0, secret)
        get_request = provider.server.get_request
        def observed_accept():
            request = get_request()
            accepted.set()
            return request
        provider.server.get_request = observed_accept
        began = time.monotonic()
        connection = None
        try:
            with provider:
                connection = socket.create_connection(("127.0.0.1", provider.server.server_port), timeout=1)
                if mode == "partial-line":
                    connection.sendall(b"GET /health HT")
                else:
                    connection.sendall(("POST /v1/chat/completions HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer " + secret +
                        "\r\nContent-Length: 100\r\n\r\n{").encode())
                if not accepted.wait(1):
                    raise RuntimeError("Owned incomplete-request barrier not reached")
            print(json.dumps({"cleanup": provider.cleanup, "listener_alive": provider.thread.is_alive(), "seconds": time.monotonic()-began}))
        finally:
            if connection is not None:
                connection.close()
        raise SystemExit(0)
    if len(sys.argv) > 1 and sys.argv[1] == "--fake-child":
        mode, receipt, root = sys.argv[2:]
        if mode == "timeout":
            time.sleep(20)
        if mode in ("secret", "overwrite-metadata"):
            print(os.environ["QUAL1_TEST_KEY"])
        if mode in ("malformed-secret", "invalid-utf8-secret"):
            prefix = b"{ malformed " if mode == "malformed-secret" else b"\xff invalid UTF8 "
            Path(receipt).write_bytes(prefix + os.environ["QUAL1_TEST_KEY"].encode())
        elif mode == "duplicate-secret":
            Path(receipt).write_text(json.dumps({"error": os.environ["QUAL1_TEST_KEY"]})[:-1] + ',"phase":"write-correct","phase":"recall"}')
        elif mode == "malformed":
            Path(receipt).write_text("not json")
        elif mode != "missing":
            data = {"schema_version": 1, "scope": "fixture", "real_model": "not_run",
                "phase": "recall" if mode in ("wrong-phase", "wrong-phase-secret") else "write-correct", "pid": os.getpid(), "run_id": "test-run",
                "runtime_root": root, "fault": "none", "status": "passed", "checks": {}}
            if mode == "wrong-phase-secret":
                data["error"] = os.environ["QUAL1_TEST_KEY"]
            if mode == "overwrite-metadata":
                data.update(credential_in_child_output=False, cleanup="forged_cleanup", process_exit=77,
                    seconds=-1, stdout="forged_stdout", stderr="forged_stderr")
            Path(receipt).write_text(json.dumps(data))
        raise SystemExit(7 if mode == "bad-exit" else 0)
    options = argparse.ArgumentParser()
    options.add_argument("--worker", type=Path)
    options.add_argument("--evidence-root", type=Path)
    args = options.parse_args()
    WORKER = args.worker
    if WORKER:
        EVIDENCE = (args.evidence_root or Path.cwd() / "qualification-evidence" / uuid.uuid4().hex[:8]).absolute()
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(NativeTests)
    else:
        suite = unittest.TestSuite(unittest.defaultTestLoader.loadTestsFromTestCase(case) for case in (OracleTests, OwnershipTests))
    outcome = unittest.TextTestRunner(verbosity=2).run(suite)
    raise SystemExit(0 if outcome.wasSuccessful() and not outcome.skipped else 1)
