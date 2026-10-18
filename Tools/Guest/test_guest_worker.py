"""Exercises real confined files; Linux process-group tests run inside the VM."""
import importlib.util
import os
from pathlib import Path
import tempfile
import threading
import time
import unittest
from unittest import mock

MODULE = Path(__file__).with_name("guest_worker.py")


class GuestWorkerTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue(MODULE.exists(), "The typed guest worker is not implemented.")
        spec = importlib.util.spec_from_file_location("guest_worker", MODULE)
        self.api = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.api)
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "workspace"
        self.root.mkdir()

    def test_file_paths_cannot_escape_workspace(self):
        for bad in ("../secret", "/etc/passwd", "a/../../secret", "", "a\\..\\secret", "a//b", "a/./b", "a/", "a\nb", "a" * 256, "é" * 1100):
            with self.assertRaises(self.api.RequestError, msg=bad):
                self.api.confined(self.root, bad)

    def test_write_requires_digest_before_overwriting(self):
        target = self.root / "code.py"
        target.write_text("original")
        request = {"operation": "write_text", "path": "code.py", "content": "replacement", "expectedDigest": "0" * 64}
        with self.assertRaises(self.api.RequestError):
            self.api.handle(request, self.root)
        self.assertEqual(target.read_text(), "original")
        import hashlib
        request["expectedDigest"] = hashlib.sha256(b"original").hexdigest()
        result = self.api.handle(request, self.root)
        self.assertTrue(result["ok"])
        self.assertEqual(target.read_text(), "replacement")

    def test_create_requires_explicit_absence_and_reads_are_bounded(self):
        result = self.api.handle({"operation": "write_text", "path": "new.txt", "content": "hello", "expectedDigest": "missing"}, self.root)
        self.assertTrue(result["ok"])
        read = self.api.handle({"operation": "read_text", "path": "new.txt"}, self.root)
        self.assertEqual(read["content"], "hello")
        with self.assertRaises(self.api.RequestError):
            self.api.handle({"operation": "write_text", "path": "new.txt", "content": "overwritten", "expectedDigest": "missing"}, self.root)

    def test_host_limits_are_enforced_before_file_mutation(self):
        request = {"operation": "write_text", "path": "new.txt", "content": "too long", "expectedDigest": "missing", "limits": {"maxWriteBytes": 3}}
        with self.assertRaises(self.api.RequestError):
            self.api.handle(request, self.root)
        self.assertFalse((self.root / "new.txt").exists())
        (self.root / "large.txt").write_text("1234")
        with self.assertRaises(self.api.RequestError):
            self.api.handle({"operation": "read_text", "path": "large.txt", "limits": {"maxReadBytes": 3}}, self.root)

    def test_host_executable_subset_cannot_be_widened(self):
        with self.assertRaisesRegex(self.api.RequestError, "executable_or_arguments_not_allowed"):
            self.api.handle({"operation": "execute", "executable": "/usr/bin/python3", "limits": {"approvedExecutables": []}}, self.root)

    def test_argument_budget_uses_utf8_bytes_and_combined_size(self):
        for arguments in (["é" * 5000], ["x" * 8192] * 9):
            with self.assertRaisesRegex(self.api.RequestError, "executable_or_arguments_not_allowed"):
                self.api.handle({"operation": "execute", "executable": "/usr/bin/python3", "arguments": arguments}, self.root)

    def test_health_checks_entire_advertised_toolchain(self):
        with mock.patch.object(self.api.os, "access", side_effect=lambda path, mode: path != "/usr/bin/g++"):
            self.assertEqual(self.api.missing_executables(), ["/usr/bin/g++"])

    def test_read_rejects_hardlink_to_another_file(self):
        outside = Path(self.temporary.name) / "private.txt"
        outside.write_text("private")
        os.link(outside, self.root / "linked.txt")
        with self.assertRaises(self.api.RequestError):
            self.api.handle({"operation": "read_text", "path": "linked.txt"}, self.root)

    def test_transport_eof_cancels_and_open_transport_does_not(self):
        read_fd, write_fd = os.pipe()
        reader = os.fdopen(read_fd, "rb", buffering=0)
        writer = os.fdopen(write_fd, "wb", buffering=0)
        self.addCleanup(reader.close)
        thread = threading.Thread(target=self.api.watch_disconnect, args=(reader,), daemon=True)
        thread.start()
        self.assertFalse(self.api.cancelled.wait(0.05))
        writer.close()
        self.assertTrue(self.api.cancelled.wait(1))
        thread.join(1)

    @unittest.skipUnless(os.name == "posix", "Guest process-group proof requires Linux; run this suite in the guest.")
    def test_eof_kills_owned_process_group(self):
        read_fd, write_fd = os.pipe()
        reader = os.fdopen(read_fd, "rb", buffering=0)
        writer = os.fdopen(write_fd, "wb", buffering=0)
        self.addCleanup(reader.close)
        threading.Thread(target=self.api.watch_disconnect, args=(reader,), daemon=True).start()
        threading.Timer(0.5, writer.close).start()
        start = time.monotonic()
        code = "import pathlib,subprocess,sys,time; child=subprocess.Popen([sys.executable,'-c','import time; time.sleep(20)']); pathlib.Path('descendant.pid').write_text(str(child.pid)); time.sleep(20)"
        result = self.api.handle({"operation": "execute", "executable": "/usr/bin/python3", "arguments": ["-c", code], "timeoutMs": 10000}, self.root)
        self.assertTrue(result["cancelled"])
        self.assertFalse(result["succeeded"])
        self.assertLess(time.monotonic() - start, 3)
        descendant = int((self.root / "descendant.pid").read_text())
        status = Path(f"/proc/{descendant}/stat")
        if status.exists():
            self.assertEqual(status.read_text().split(")", 1)[1].strip().split()[0], "Z", "A descendant survived transport cancellation.")

    @unittest.skipUnless(os.name == "posix", "Guest process deadlines and output proof require Linux.")
    def test_process_deadline_and_output_bound(self):
        result = self.api.handle({"operation": "execute", "executable": "/usr/bin/python3", "arguments": ["-c", "import time; print('x'*10000,flush=True); time.sleep(20)"], "timeoutMs": 150, "limits": {"maxOutputBytes": 100}}, self.root)
        self.assertTrue(result["timedOut"])
        self.assertTrue(result["outputTruncated"])
        self.assertLessEqual(len(result["standardOutput"].encode()) + len(result["standardError"].encode()), 100)
        self.assertFalse(result["succeeded"])

    @unittest.skipUnless(os.name == "posix", "Detached descendant cleanup requires the Linux guest.")
    def test_cancel_reaps_detached_descendant_before_delayed_write(self):
        read_fd, write_fd = os.pipe()
        reader = os.fdopen(read_fd, "rb", buffering=0)
        writer = os.fdopen(write_fd, "wb", buffering=0)
        self.addCleanup(reader.close)
        threading.Thread(target=self.api.watch_disconnect, args=(reader,), daemon=True).start()
        threading.Timer(0.5, writer.close).start()
        detached = "import pathlib,time; time.sleep(1.5); pathlib.Path('late-write.txt').write_text('escaped')"
        code = "import pathlib,subprocess,sys,time; child=subprocess.Popen([sys.executable,'-c'," + repr(detached) + "],start_new_session=True); pathlib.Path('detached.pid').write_text(str(child.pid)); time.sleep(20)"
        result = self.api.handle({"operation": "execute", "executable": "/usr/bin/python3", "arguments": ["-c", code], "timeoutMs": 10000}, self.root)
        self.assertTrue(result["cancelled"])
        self.assertTrue(result["cleanupConfirmed"])
        descendant = int((self.root / "detached.pid").read_text())
        self.assertFalse(Path(f"/proc/{descendant}").exists())
        time.sleep(1.6)
        self.assertFalse((self.root / "late-write.txt").exists())

    def test_execute_requires_allowlisted_executable_and_no_shell(self):
        with self.assertRaises(self.api.RequestError):
            self.api.handle({"operation": "execute", "executable": "/bin/sh", "arguments": ["-c", "whoami"], "workingDirectory": "."}, self.root)
        with self.assertRaises(self.api.RequestError):
            self.api.handle({"operation": "execute", "executable": "/usr/bin/python3", "arguments": "-c print(1)", "workingDirectory": "."}, self.root)

    def test_symlink_escape_is_rejected(self):
        outside = Path(self.temporary.name) / "outside"
        outside.mkdir()
        link = self.root / "link"
        try:
            link.symlink_to(outside, target_is_directory=True)
        except OSError:
            self.skipTest("This host does not permit symlink creation.")
        with self.assertRaises(self.api.RequestError):
            self.api.confined(self.root, "link/secret")


if __name__ == "__main__":
    unittest.main()
