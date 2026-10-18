"""Offline bootstrap regressions. Never invokes VMware or downloads an image."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock
import sys


MODULE = Path(__file__).with_name("vmware_ubuntu.py")


class BootstrapTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue(MODULE.exists(), "The automatic VMware bootstrap is not implemented.")
        spec = importlib.util.spec_from_file_location("vmware_ubuntu", MODULE)
        self.api = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.api)
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "owned"

    def test_unknown_existing_directory_is_never_adopted(self):
        self.root.mkdir()
        original = self.root / "personal.vmx"
        original.write_text("personal VM")
        with self.assertRaises(self.api.BootstrapError):
            self.api.enroll(self.root, "companion-one")
        self.assertEqual(original.read_text(), "personal VM")

    def test_enrollment_is_immutable_and_owner_bound(self):
        first = self.api.enroll(self.root, "companion-one")
        before = (self.root / "manifest.json").read_bytes()
        second = self.api.enroll(self.root, "companion-one")
        self.assertEqual(first, second)
        self.assertEqual(before, (self.root / "manifest.json").read_bytes())
        with self.assertRaises(self.api.BootstrapError):
            self.api.enroll(self.root, "companion-two")

    def test_manifest_cannot_redirect_vm_or_change_resources(self):
        self.api.enroll(self.root, "companion-one")
        path = self.root / "manifest.json"
        manifest = json.loads(path.read_text())
        manifest["vmx"] = "../personal.vmx"
        path.write_text(json.dumps(manifest))
        with self.assertRaises(self.api.BootstrapError):
            self.api.read_manifest(self.root, "companion-one")

    def test_hash_mismatch_never_publishes_cached_image(self):
        self.root.mkdir()
        target = self.root / "image.vmdk"
        with self.assertRaises(self.api.BootstrapError):
            self.api.download_verified("https://cloud-images.ubuntu.com/test.vmdk", target,
                hashlib.sha256(b"expected").hexdigest(), opener=lambda _: FakeResponse(b"bad"))
        self.assertFalse(target.exists())

    def test_download_accepts_only_expected_hash_and_https_canonical_host(self):
        self.root.mkdir()
        target = self.root / "image.vmdk"
        self.api.download_verified("https://cloud-images.ubuntu.com/test.vmdk", target,
            hashlib.sha256(b"expected").hexdigest(), opener=lambda _: FakeResponse(b"expected"))
        self.assertEqual(target.read_bytes(), b"expected")
        with self.assertRaises(self.api.BootstrapError):
            self.api.download_verified("http://example.org/image", target, "0" * 64)

    def test_cloud_init_is_key_only_and_helper_forced(self):
        manifest = self.api.enroll(self.root, "companion-one")
        text = self.api.cloud_config(manifest, "ssh-ed25519 AAAA fixture", "HOST-PRIVATE", "ssh-ed25519 BBBB host")
        self.assertIn("restrict,command=", text)
        self.assertIn("ssh_pwauth: false", text)
        self.assertIn("lock_passwd: true", text)
        self.assertNotIn("sudo: ALL", text)
        self.assertIn("/opt/revia/guest_worker.py", text)
        self.assertIn(manifest["environmentId"], text)

    def test_ssh_never_uses_ambient_config_or_unpinned_host(self):
        manifest = self.api.enroll(self.root, "companion-one")
        command = self.api.ssh_command(self.root, manifest, "192.168.100.7", "ssh.exe")
        self.assertIn("StrictHostKeyChecking=yes", command)
        self.assertIn("IdentitiesOnly=yes", command)
        self.assertIn("ForwardAgent=no", command)
        self.assertEqual(command[command.index("-F") + 1], "NUL" if self.api.os.name == "nt" else "/dev/null")
        self.assertNotIn("StrictHostKeyChecking=no", command)
        with self.assertRaises(self.api.BootstrapError):
            self.api.ssh_command(self.root, manifest, "-oProxyCommand=bad", "ssh.exe")

    def test_cloud_init_installs_and_starts_guest_discovery_dependencies(self):
        manifest = self.api.enroll(self.root, "companion-one")
        config = self.api.cloud_config(manifest, "ssh-ed25519 AAAA fixture", "private", "ssh-ed25519 BBBB host")
        packages = next(line for line in config.splitlines() if line.startswith("packages: "))
        self.assertIn("open-vm-tools", packages)
        self.assertIn("openssh-server", packages)
        commands = config.split("\nruncmd:\n", 1)[1]
        self.assertIn("[systemctl, enable, --now, open-vm-tools]", commands)
        self.assertLess(commands.index("systemctl, enable, --now, open-vm-tools"),
                        commands.index("touch /var/lib/revia/bootstrap-ready"))
        self.assertIn("systemctl is-active --quiet open-vm-tools", commands)

    def test_cloud_init_prepares_the_interpreter_sandbox(self):
        manifest = self.api.enroll(self.root, "companion-one")
        config = self.api.cloud_config(manifest, "ssh-ed25519 AAAA fixture", "private", "ssh-ed25519 BBBB host")
        packages = next(line for line in config.splitlines() if line.startswith("packages: "))
        for package in ("python3-numpy", "python3-matplotlib", "python3-sympy"):
            self.assertIn(package, packages)
        self.assertIn("kernel.apparmor_restrict_unprivileged_userns = 0", config)
        commands = config.split("\nruncmd:\n", 1)[1]
        self.assertLess(commands.index("[sysctl, --system]"), commands.index("touch /var/lib/revia/bootstrap-ready"))
        self.assertNotIn("sudo", config)

    def test_ready_requires_matching_environment_and_valid_boot_epoch(self):
        manifest = self.api.enroll(self.root, "companion-one")
        good = {"schemaVersion": 1, "ok": True, "ready": True,
                "environmentId": manifest["environmentId"], "bootId": "a37bb9b4-59c8-4ec6-aa8d-d5a238b9e823"}
        self.assertTrue(self.api.valid_health(good, manifest))
        for changes in ({"environmentId": "another"}, {"ready": False}, {"bootId": ""}, {"ok": False}):
            self.assertFalse(self.api.valid_health({**good, **changes}, manifest))

    def test_vm_settings_disable_host_integration_and_bound_resources(self):
        manifest = self.api.enroll(self.root, "companion-one")
        entries = self.api.vm_entries(self.root, manifest)
        self.assertEqual(entries["memsize"], "2048")
        self.assertEqual(entries["numvcpus"], "2")
        self.assertEqual(entries["ethernet0.connectionType"], "nat")
        self.assertEqual(entries["isolation.tools.copy.disable"], "TRUE")
        self.assertEqual(entries["isolation.tools.paste.disable"], "TRUE")
        self.assertEqual(entries["isolation.tools.dnd.disable"], "TRUE")
        self.assertEqual(entries["sharedFolder.maxNum"], "0")
        self.assertFalse(any("hostPath" in key for key in entries))

    def test_vm_start_defers_when_host_memory_is_low(self):
        with mock.patch.object(self.api, "available_memory", return_value=2 * 1024 ** 3):
            with self.assertRaisesRegex(self.api.BootstrapError, "3 GiB"):
                self.api.check_start_resources(self.root)

    def test_vm_admission_uses_current_headroom_on_larger_hosts(self):
        disk = mock.Mock(free=6 * 1024 ** 3)
        for available_gib in (3, 8, 32, 64):
            with self.subTest(available_gib=available_gib), \
                 mock.patch.object(self.api, "available_memory", return_value=available_gib * 1024 ** 3), \
                 mock.patch.object(self.api.shutil, "disk_usage", return_value=disk):
                self.api.check_start_resources(self.root)
        disk.free = 5 * 1024 ** 3
        with mock.patch.object(self.api, "available_memory", return_value=64 * 1024 ** 3), \
             mock.patch.object(self.api.shutil, "disk_usage", return_value=disk):
            with self.assertRaisesRegex(self.api.BootstrapError, "6 GiB"):
                self.api.check_start_resources(self.root)

    def test_vmx_rejects_extra_host_devices_and_redirected_artifacts(self):
        manifest = self.api.enroll(self.root, "companion-one")
        (self.root / "machine").mkdir()
        vmx = self.root / "machine/revia.vmx"
        baseline = "\n".join(key + ' = "' + value + '"' for key, value in self.api.vm_entries(self.root, manifest).items()) + "\n"
        for extra in ('scsi0:1.fileName = "C:/private.vmdk"', 'ethernet1.present = "TRUE"',
                      'serial0.fileName = "C:/private.txt"', 'nvram = "C:/private.nvram"',
                      'sharedFolder0.present = "TRUE"', 'MEMSIZE = "8192"'):
            vmx.write_text(baseline + extra + "\n")
            with self.assertRaises(self.api.BootstrapError, msg=extra):
                self.api.validate_vmx(self.root, manifest)

    def test_forced_guest_python_disables_user_site_startup(self):
        manifest = self.api.enroll(self.root, "companion-one")
        config = self.api.cloud_config(manifest, "ssh-ed25519 AAAA fixture", "private", "ssh-ed25519 BBBB host")
        self.assertEqual(config.count("/usr/bin/python3 -I -B /opt/revia/guest_worker.py"), 2)
        command = self.api.ssh_command(self.root, manifest, "192.168.100.7", "ssh.exe")
        self.assertEqual(command[-4:], ["/usr/bin/python3", "-I", "-B", "/opt/revia/guest_worker.py"])

    def test_cancelled_partial_setup_stops_idempotently_without_vm_command(self):
        self.api.enroll(self.root, "companion-one")
        with mock.patch.object(self.api, "command") as command:
            result = self.api.stop_guest(self.root, "companion-one", {})
        self.assertEqual(result["state"], "stopped")
        self.assertTrue(result["ok"])
        command.assert_not_called()

    def test_existing_unrecorded_vmx_is_not_reconfigured(self):
        self.api.enroll(self.root, "companion-one")
        (self.root / "machine").mkdir()
        vmx = self.root / "machine/revia.vmx"
        vmx.write_text('displayName = "personal"\n')
        with mock.patch.object(self.api, "secure_directory"), mock.patch.object(self.api, "command") as command:
            with self.assertRaisesRegex(self.api.BootstrapError, "ownership|unrecorded"):
                self.api.ensure(self.root, "companion-one", {})
        command.assert_not_called()
        self.assertIn("personal", vmx.read_text())

    def test_provision_is_idempotent_and_publishes_readiness_only_after_health(self):
        calls = []
        seeds = []
        powered = [False]
        installed = {name: name for name in ("vmrun", "vmcli", "disk", "iso", "keygen", "ssh")}
        vmx = self.root / "machine/revia.vmx"
        def fake_command(argv, timeout=60, input_data=None, keep_stdin=False):
            argv = [str(item) for item in argv]
            calls.append(argv)
            if argv[0] == "keygen":
                key = Path(argv[-1]); key.write_text("fixture-private"); key.with_suffix(".pub").write_text("ssh-ed25519 AAAA fixture")
            elif argv[0] == "iso":
                seeds.append((Path(argv[-1]) / "user-data").read_text())
                Path(argv[argv.index("-output") + 1]).write_bytes(b"seed")
            elif argv[0] == "disk" and argv[1] == "-r":
                Path(argv[-1]).write_bytes(b"disk")
            elif argv[0] == "disk" and argv[1] == "-n":
                Path(argv[2]).rename(argv[3])
            elif argv[:3] == ["vmcli", "VM", "Create"]:
                vmx.write_text('displayName = "revia"\n')
            elif argv[0] == "vmcli" and argv[2:4] == ["ConfigParams", "SetEntry"]:
                with vmx.open("a") as output:
                    output.write(argv[4] + ' = "' + argv[5] + '"\n')
            elif argv[0] == "vmrun" and argv[3] == "list":
                return "Total running VMs: " + ("1\n" + str(vmx) if powered[0] else "0")
            elif argv[0] == "vmrun" and argv[3] == "start":
                self.assertEqual(argv[-1], "nogui"); powered[0] = True
            return ""
        def health(*args):
            return {"state": "booting", "code": "guest_bootstrap_pending", "guestReady": False}
        with mock.patch.object(self.api, "command", side_effect=fake_command), \
             mock.patch.object(self.api, "download_verified", side_effect=lambda url, path, digest: path.write_bytes(b"image")), \
             mock.patch.object(self.api, "secure_directory"), \
             mock.patch.object(self.api, "available_memory", return_value=8 * 1024 ** 3), \
             mock.patch.object(self.api.shutil, "disk_usage", return_value=type("Disk", (), {"free": 100 * 1024 ** 3})()), \
             mock.patch.object(self.api, "status", side_effect=health):
            first = self.api.ensure(self.root, "companion-one", installed)
            marker = (self.root / "manifest.json").read_bytes()
            second = self.api.ensure(self.root, "companion-one", installed)
        self.assertFalse(first["guestReady"])
        self.assertFalse(second["guestReady"])
        self.assertEqual(marker, (self.root / "manifest.json").read_bytes())
        self.assertEqual(sum(call[:3] == ["vmcli", "VM", "Create"] for call in calls), 1)
        self.assertEqual(sum(call[0] == "disk" and call[1] == "-n" for call in calls), 1)
        self.assertEqual(sum(call[0] == "vmrun" and call[3] == "start" for call in calls), 1)
        # First boot needs the guest host key in the seed, but the host keeps only the
        # pinned public half and no plaintext copy of the seed.
        self.assertEqual(len(seeds), 1)
        self.assertIn("fixture-private", seeds[0])
        self.assertFalse((self.root / "keys/host").exists())
        self.assertTrue((self.root / "keys/host.pub").exists())
        self.assertTrue((self.root / "keys/client").exists())
        self.assertFalse((self.root / "seed/user-data").exists())

    def test_seed_drops_the_guest_host_private_key_after_first_boot(self):
        manifest = self.api.enroll(self.root, "companion-one")
        (self.root / "keys").mkdir()
        (self.root / "keys/client.pub").write_text("ssh-ed25519 AAAA fixture")
        seeds = []
        def fake_command(argv, timeout=60, input_data=None, keep_stdin=False):
            argv = [str(item) for item in argv]
            if argv[0] == "iso":
                seeds.append((Path(argv[-1]) / "user-data").read_text())
                Path(argv[argv.index("-output") + 1]).write_bytes(b"scrubbed")
            return ""
        installed = {"iso": "iso"}
        with mock.patch.object(self.api, "command", side_effect=fake_command):
            self.api.scrub_seed(self.root, manifest, installed)
            self.assertEqual(seeds, [], "The seed was rebuilt before the guest had booted with its host key.")
            self.api.record(self.root, "guest-ready.json", manifest)
            self.api.scrub_seed(self.root, manifest, installed)
            self.api.scrub_seed(self.root, manifest, installed)
        self.assertEqual(len(seeds), 1)
        self.assertNotIn("ssh_keys", seeds[0])
        self.assertNotIn("ed25519_private", seeds[0])
        self.assertIn("restrict,command=", seeds[0])
        self.assertEqual((self.root / "seed.iso").read_bytes(), b"scrubbed")
        self.assertFalse((self.root / "seed/user-data").exists())

    def test_interrupted_setup_never_reuses_an_unpaired_host_key(self):
        self.api.enroll(self.root, "companion-one")
        (self.root / "keys").mkdir()
        (self.root / "keys/host.pub").write_text("ssh-ed25519 STALE fixture")
        installed = {name: name for name in ("vmrun", "vmcli", "disk", "iso", "keygen", "ssh")}
        generated = []
        def fake_command(argv, timeout=60, input_data=None, keep_stdin=False):
            argv = [str(item) for item in argv]
            if argv[0] == "keygen":
                generated.append(Path(argv[-1]).name)
                key = Path(argv[-1]); key.write_text("fixture-private"); key.with_suffix(".pub").write_text("ssh-ed25519 FRESH fixture")
            elif argv[0] == "iso":
                raise self.api.BootstrapError("cancelled")
            return ""
        with mock.patch.object(self.api, "command", side_effect=fake_command), \
             mock.patch.object(self.api, "download_verified", side_effect=lambda url, path, digest: path.write_bytes(b"image")), \
             mock.patch.object(self.api, "secure_directory"), \
             mock.patch.object(self.api.shutil, "disk_usage", return_value=type("Disk", (), {"free": 100 * 1024 ** 3})()):
            with self.assertRaises(self.api.BootstrapError):
                self.api.ensure(self.root, "companion-one", installed)
        self.assertEqual(sorted(generated), ["client", "host"])
        self.assertNotIn("STALE", (self.root / "keys/known_hosts").read_text())
        self.assertFalse((self.root / "seed/user-data").exists())

    def test_ssh_input_stays_open_until_response(self):
        script = "import sys; value=sys.stdin.buffer.readline(); print(value.decode().strip(), flush=True)"
        self.assertEqual(self.api.command([sys.executable, "-c", script], input_data=b"receipt\n", keep_stdin=True).strip(), "receipt")

    def test_transport_failure_after_dispatch_has_uncertain_effect(self):
        manifest = self.api.enroll(self.root, "companion-one")
        request = {"schemaVersion": 1, "requestId": "one", "environmentId": manifest["environmentId"],
                   "bootId": "old-boot", "operation": "write_text", "path": "x", "content": "x", "expectedDigest": "missing"}
        with mock.patch.object(self.api, "validate_vmx", return_value=self.root / "machine/revia.vmx"), \
             mock.patch.object(self.api, "running", return_value=True), \
             mock.patch.object(self.api, "command", side_effect=["192.168.100.7", self.api.BootstrapError("command_failed")]):
            result = self.api.guest_request(self.root, manifest, {"vmrun": "vmrun", "ssh": "ssh"}, request)
        self.assertTrue(result["uncertainEffect"])
        self.assertTrue(result["attempted"])
        self.assertFalse(result["succeeded"])
        self.assertEqual(result["code"], "command_failed")

    def test_initial_guest_address_discovery_waits_with_bounded_vmrun(self):
        manifest = self.api.enroll(self.root, "companion-one")
        request = {"schemaVersion": 1, "requestId": "first-health", "environmentId": manifest["environmentId"],
                   "operation": "health"}
        receipt = {**request, "ok": True, "ready": True, "bootId": "a37bb9b4-59c8-4ec6-aa8d-d5a238b9e823"}
        def command(argv, timeout=60, input_data=None, keep_stdin=False):
            if argv[0] == "vmrun":
                if "-wait" not in argv:
                    raise self.api.BootstrapError("command_failed", "VMware has not initialized guest address discovery.")
                self.assertEqual(timeout, 15)
                return "192.168.100.7"
            return json.dumps(receipt)
        with mock.patch.object(self.api, "validate_vmx", return_value=self.root / "machine/revia.vmx"), \
             mock.patch.object(self.api, "running", return_value=True), \
             mock.patch.object(self.api, "command", side_effect=command):
            try:
                result = self.api.guest_request(self.root, manifest, {"vmrun": "vmrun", "ssh": "ssh"}, request)
            except self.api.BootstrapError as error:
                self.fail("Initial guest discovery must wait within its deadline: " + error.code)
        self.assertTrue(result["ready"])
        self.assertTrue(result["receiptVerified"])


class FakeResponse:
    def __init__(self, data):
        import io
        self.stream = io.BytesIO(data)
    def __enter__(self):
        return self
    def __exit__(self, *args):
        self.stream.close()
    def read(self, size):
        return self.stream.read(size)
    def geturl(self):
        return "https://cloud-images.ubuntu.com/test.vmdk"


if __name__ == "__main__":
    unittest.main()
