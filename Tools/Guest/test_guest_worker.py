"""Exercises real confined files; Linux process-group tests run inside the VM."""
import importlib.util
import os
import socket
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

    def run_python(self, code, timeout=10000, **limits):
        request = {"operation": "run_python", "runId": "run-" + os.urandom(6).hex(), "code": code, "timeoutMs": timeout}
        if limits:
            request["limits"] = limits
        return self.api.handle(request, self.root)

    @unittest.skipUnless(os.name == "posix", "The interpreter sandbox requires the Linux guest.")
    def test_interpreter_has_no_network(self):
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.addCleanup(listener.close)
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        listener.settimeout(0.2)
        port = listener.getsockname()[1]
        code = ("import socket\n"
                "interfaces = sorted(l.split(':')[0].strip() for l in open('/proc/self/net/dev').read().splitlines()[2:])\n"
                "print('interfaces', interfaces)\n"
                f"for address in (('127.0.0.1', {port}), ('10.0.2.2', 80), ('1.1.1.1', 443)):\n"
                "    try:\n"
                "        socket.create_connection(address, timeout=2).close()\n"
                "        print('connected', address)\n"
                "    except OSError as error:\n"
                "        print('blocked', type(error).__name__)\n")
        result = self.run_python(code)
        self.assertTrue(result["succeeded"], result)
        self.assertTrue(result["networkIsolated"])
        self.assertIn("interfaces ['lo']", result["standardOutput"])
        self.assertEqual(result["standardOutput"].count("blocked"), 3, result["standardOutput"])
        self.assertNotIn("connected", result["standardOutput"])
        with self.assertRaises(socket.timeout):
            listener.accept()

    @unittest.skipUnless(os.name == "posix", "The interpreter sandbox requires the Linux guest.")
    def test_interpreter_runs_under_kernel_limits(self):
        code = ("import resource\n"
                "for name in ('RLIMIT_AS', 'RLIMIT_CPU', 'RLIMIT_FSIZE', 'RLIMIT_NPROC', 'RLIMIT_CORE'):\n"
                "    print(name, resource.getrlimit(getattr(resource, name)))\n"
                "try:\n"
                "    block = bytearray(2 << 30)\n"
                "    print('allocated')\n"
                "except MemoryError:\n"
                "    print('memory refused')\n")
        result = self.run_python(code, timeout=3000)
        self.assertTrue(result["succeeded"], result)
        output = result["standardOutput"]
        self.assertIn(f"RLIMIT_AS ({1 << 30}, {1 << 30})", output)
        self.assertIn("RLIMIT_CPU (3, 4)", output)
        self.assertIn(f"RLIMIT_FSIZE ({16 << 20}, {16 << 20})", output)
        self.assertIn("RLIMIT_CORE (0, 0)", output)
        self.assertIn("memory refused", output)
        self.assertNotIn("allocated", output)
        start = time.monotonic()
        spin = self.run_python("while True:\n    pass\n", timeout=1500)
        self.assertFalse(spin["succeeded"])
        self.assertTrue(spin["timedOut"] or spin["exitCode"] < 0, spin)
        self.assertTrue(spin["cleanupConfirmed"])
        self.assertLess(time.monotonic() - start, 6)

    @unittest.skipUnless(os.name == "posix", "The interpreter sandbox requires the Linux guest.")
    def test_each_run_gets_a_fresh_scratch_directory_that_is_removed(self):
        first = self.run_python("import os, pathlib\npathlib.Path('left-behind.txt').write_text('x')\nprint(os.getcwd())\n")
        self.assertTrue(first["succeeded"], first)
        self.assertIn("/sandbox/" + first["runId"], first["standardOutput"])
        second = self.run_python("import os\nprint(sorted(os.listdir('.')), os.environ['HOME'] == os.getcwd())\n")
        self.assertIn("['main.py', 'out', 'tmp'] True", second["standardOutput"])
        self.assertEqual(list((self.root / "sandbox").iterdir()), [], "A run's scratch directory outlived it.")
        (self.root / "sandbox" / "run-existing1").mkdir()
        (self.root / "sandbox" / "run-existing1" / "keep.txt").write_text("keep")
        with self.assertRaisesRegex(self.api.RequestError, "run_scratch_exists"):
            self.api.handle({"operation": "run_python", "runId": "run-existing1", "code": "print(1)"}, self.root)
        self.assertEqual((self.root / "sandbox" / "run-existing1" / "keep.txt").read_text(), "keep")
        for bad in ("../escape", "UPPER-case1", "short", "a/b-cdefgh"):
            with self.assertRaisesRegex(self.api.RequestError, "invalid_run_id"):
                self.api.handle({"operation": "run_python", "runId": bad, "code": "print(1)"}, self.root)

    @unittest.skipUnless(os.name == "posix", "The interpreter sandbox requires the Linux guest.")
    def test_interpreter_output_is_bounded(self):
        result = self.run_python("import sys\nprint('x' * 200000)\nprint('y' * 5000, file=sys.stderr)\n", maxOutputBytes=1000)
        self.assertTrue(result["outputTruncated"])
        self.assertLessEqual(len(result["standardOutput"].encode()) + len(result["standardError"].encode()), 1000)
        with self.assertRaisesRegex(self.api.RequestError, "code_too_large_or_invalid"):
            self.run_python("print(1)\n" * 100, maxWriteBytes=50)

    @unittest.skipUnless(os.name == "posix", "The interpreter sandbox requires the Linux guest.")
    def test_generated_files_come_back_bounded_and_typed(self):
        code = ("import os, struct, zlib\n"
                "def chunk(kind, data):\n"
                "    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))\n"
                "png = b'\\x89PNG\\r\\n\\x1a\\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 1, 1, 8, 2, 0, 0, 0))"
                " + chunk(b'IDAT', zlib.compress(b'\\x00\\xff\\x00\\x00')) + chunk(b'IEND', b'')\n"
                "open('out/plot.png', 'wb').write(png)\n"
                "open('out/data.csv', 'w').write('x,y\\n1,2\\n')\n"
                "open('out/run.sh', 'w').write('echo no')\n"
                "os.symlink('/etc/passwd', 'out/link.txt')\n"
                "open('out/large.png', 'wb').write(b'0' * 600000)\n")
        result = self.run_python(code)
        self.assertTrue(result["succeeded"], result)
        artifacts = {item["name"]: item for item in result["artifacts"]}
        self.assertEqual(set(artifacts), {"data.csv", "plot.png"})
        import base64
        import hashlib
        png = base64.b64decode(artifacts["plot.png"]["contentBase64"])
        self.assertTrue(png.startswith(b"\x89PNG\r\n\x1a\n"))
        self.assertEqual(hashlib.sha256(png).hexdigest(), artifacts["plot.png"]["sha256"])
        self.assertEqual(base64.b64decode(artifacts["data.csv"]["contentBase64"]), b"x,y\n1,2\n")
        omitted = {item["name"]: item["reason"] for item in result["artifactsOmitted"]}
        self.assertEqual(omitted["run.sh"], "unsupported_name_or_type")
        self.assertEqual(omitted["link.txt"], "not_a_bounded_regular_file")
        self.assertEqual(omitted["large.png"], "not_a_bounded_regular_file")
        self.assertLessEqual(sum(item["bytes"] for item in result["artifacts"]), self.api.MAX_ARTIFACT_BYTES)

    def test_interpreter_never_runs_when_isolation_fails(self):
        marker = Path(self.temporary.name) / "ran.txt"
        with mock.patch.object(self.api, "unshare", side_effect=OSError("namespaces unavailable")):
            with self.assertRaisesRegex(self.api.RequestError, "sandbox_unavailable|linux_guest_required"):
                self.run_python(f"open({str(marker)!r}, 'w').write('ran')\n")
        self.assertFalse(marker.exists(), "Code ran without its network isolation.")
        with self.assertRaisesRegex(self.api.RequestError, "executable_or_arguments_not_allowed"):
            self.run_python("print(1)", approvedExecutables=["/usr/bin/git"])

    @unittest.skipUnless(os.name == "posix", "Health probes the Linux sandbox.")
    def test_health_probe_reports_whether_runs_can_be_isolated(self):
        self.assertTrue(self.api.sandbox_probe())
        with mock.patch.object(self.api, "unshare", side_effect=OSError("namespaces unavailable")):
            self.assertFalse(self.api.sandbox_probe())

    @unittest.skipUnless(os.name == "posix", "Search runs in the Linux sandbox.")
    def test_search_finds_lines_and_meets_its_deadline(self):
        project = self.root / "projects" / "demo"
        (project / "src").mkdir(parents=True)
        (project / "src" / "main.py").write_text("def add(a, b):\n    return a + b\n# TODO: subtract\n")
        (project / ".hidden").mkdir()
        (project / ".hidden" / "secret.py").write_text("TODO hidden\n")
        (project / "blob.bin").write_bytes(b"TODO\x00binary")
        found = self.api.handle({"operation": "search_text", "path": "projects/demo", "pattern": "TODO"}, self.root)
        self.assertTrue(found["succeeded"], found)
        self.assertIn("src/main.py:3:# TODO: subtract", found["standardOutput"])
        self.assertNotIn("hidden", found["standardOutput"])
        self.assertNotIn("blob.bin", found["standardOutput"])
        fixed = self.api.handle({"operation": "search_text", "path": "projects/demo", "pattern": "a + b", "fixedStrings": True}, self.root)
        self.assertIn("src/main.py:2:", fixed["standardOutput"])
        invalid = self.api.handle({"operation": "search_text", "path": "projects/demo", "pattern": "("}, self.root)
        self.assertFalse(invalid["succeeded"])
        self.assertIn("invalid pattern", invalid["standardError"])
        (project / "slow.txt").write_text("a" * 40 + "!\n")
        start = time.monotonic()
        slow = self.api.handle({"operation": "search_text", "path": "projects/demo", "pattern": "(a+)+$", "timeoutMs": 800}, self.root)
        self.assertTrue(slow["timedOut"] or slow["exitCode"] < 0, slow)
        self.assertLess(time.monotonic() - start, 5)
        with self.assertRaisesRegex(self.api.RequestError, "invalid_search"):
            self.api.handle({"operation": "search_text", "path": "projects/demo", "pattern": "x", "maxMatches": 0}, self.root)

    @unittest.skipUnless(os.name == "posix", "Imports use Linux directory descriptors.")
    def test_shared_project_is_staged_then_replaces_the_previous_copy(self):
        def batch(share, files, publish=None):
            request = {"operation": "import_files", "path": "projects/.incoming-" + share, "files": files}
            if publish:
                request["publish"] = publish
            return self.api.handle(request, self.root)
        batch("share-00000001", [{"path": "src/a.py", "content": "old\n"}, {"path": "stale.txt", "content": "stale"}], "demo")
        batch("share-00000002", [{"path": "src/a.py", "content": "new\n"}])
        self.assertEqual((self.root / "projects/demo/src/a.py").read_text(), "old\n", "A share was visible before it was published.")
        result = batch("share-00000002", [{"path": "README.md", "content": "hello"}], "demo")
        self.assertTrue(result["succeeded"])
        self.assertEqual((self.root / "projects/demo/src/a.py").read_text(), "new\n")
        self.assertFalse((self.root / "projects/demo/stale.txt").exists())
        self.assertEqual(sorted(path.name for path in (self.root / "projects").iterdir()), ["demo"])
        for bad in ("../escape.txt", "/etc/passwd", "a/../../b", "a\\b", "c:x"):
            with self.assertRaisesRegex(self.api.RequestError, "invalid_guest_path"):
                batch("share-00000003", [{"path": bad, "content": "x"}])
        with self.assertRaisesRegex(self.api.RequestError, "guest_file_exists"):
            batch("share-00000004", [{"path": "same.txt", "content": "1"}, {"path": "same.txt", "content": "2"}])
        with self.assertRaisesRegex(self.api.RequestError, "invalid_project_name"):
            batch("share-00000005", [], "../outside")
        with self.assertRaisesRegex(self.api.RequestError, "invalid_import_path"):
            self.api.handle({"operation": "import_files", "path": "projects/demo", "files": []}, self.root)
        outside = Path(self.temporary.name) / "outside"
        outside.mkdir()
        (self.root / "projects" / ".incoming-share-00000006").symlink_to(outside, target_is_directory=True)
        with self.assertRaisesRegex(self.api.RequestError, "invalid_guest_directory"):
            batch("share-00000006", [{"path": "x.txt", "content": "x"}])
        self.assertEqual(list(outside.iterdir()), [])

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
