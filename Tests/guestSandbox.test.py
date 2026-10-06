from __future__ import annotations

import importlib.util
import json
import os
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path


SOURCE = Path(__file__).resolve().parents[1] / "Tools" / "Guest" / "windows_sandbox.py"
SPEC = importlib.util.spec_from_file_location("windows_sandbox", SOURCE)
assert SPEC is not None and SPEC.loader is not None
GUEST = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GUEST)


class GuestSandboxTests(unittest.TestCase):
    def setUp(self):
        self.fixture = tempfile.TemporaryDirectory(prefix="revia-guest-")
        self.addCleanup(self.fixture.cleanup)
        self.root = Path(self.fixture.name)
        self.workspace = self.root / "input & selected"
        self.artifacts = self.root / "runs"
        self.workspace.mkdir()
        self.artifacts.mkdir()
        (self.workspace / "keep.txt").write_text("unchanged", encoding="utf-8")

    def prepare(self, **kwargs):
        return GUEST.prepare(self.workspace, self.artifacts, **kwargs)

    def test_default_mounts_and_disabled_devices(self):
        prepared = self.prepare()
        root = ET.parse(prepared["configuration"]["path"]).getroot()
        for option in ("Networking", "vGPU", "ClipboardRedirection", "AudioInput", "VideoInput", "PrinterRedirection"):
            self.assertEqual(root.findtext(option), "Disable")
        self.assertEqual(root.findtext("ProtectedClient"), "Enable")
        mounts = root.findall("./MappedFolders/MappedFolder")
        self.assertEqual(len(mounts), 3)
        self.assertEqual(mounts[0].findtext("HostFolder"), str(self.workspace.resolve()))
        self.assertEqual([mount.findtext("ReadOnly") for mount in mounts], ["true", "true", "false"])
        self.assertEqual([mount.findtext("SandboxFolder") for mount in mounts],
                         [r"C:\Revia\Input", r"C:\Revia\Control", r"C:\Revia\Artifacts"])
        self.assertIn("&amp;", Path(prepared["configuration"]["path"]).read_text(encoding="utf-8"))
        self.assertNotIn(str(self.workspace), root.findtext("./LogonCommand/Command"))
        self.assertFalse(prepared["vmIsolationVerified"])
        self.assertFalse(prepared["guestControlReady"])
        self.assertEqual((self.workspace / "keep.txt").read_text(encoding="utf-8"), "unchanged")

    def test_network_is_explicit_and_gpu_remains_off(self):
        prepared = self.prepare(network=True)
        root = ET.parse(prepared["configuration"]["path"]).getroot()
        self.assertEqual(root.findtext("Networking"), "Enable")
        self.assertEqual(root.findtext("vGPU"), "Disable")

    def test_unique_runs_do_not_overwrite_artifacts(self):
        first = self.prepare()
        receipt = Path(first["artifactDirectory"]) / "retain.txt"
        receipt.write_text("owned output", encoding="utf-8")
        second = self.prepare()
        self.assertNotEqual(first["runId"], second["runId"])
        self.assertEqual(receipt.read_text(encoding="utf-8"), "owned output")

    def test_rejects_overlapping_mounts_and_drive_root(self):
        with self.assertRaises(ValueError):
            GUEST.prepare(self.workspace, self.workspace)
        nested = self.workspace / "outputs"
        nested.mkdir()
        with self.assertRaises(ValueError):
            GUEST.prepare(self.workspace, nested)
        with self.assertRaises(ValueError):
            GUEST.prepare(self.root, self.artifacts)
        with self.assertRaises(ValueError):
            GUEST.prepare(Path(self.root.anchor), self.artifacts)

    def test_rejects_relative_and_missing_input(self):
        for value in ("relative", str(self.root / "missing")):
            with self.assertRaises(ValueError):
                GUEST.prepare(Path(value), self.artifacts)

    def test_rejects_windows_expansion_devices_and_ambiguous_paths(self):
        for value in (r"C:\%USERPROFILE%\input", r"\\server\share\input", r"\\?\C:\input",
                      r"C:\input\..\other", r"C:\input.", r"C:\CON\input", "C:\\input\nname",
                      r"C:\input:stream", r"C:\input\ "):
            with self.subTest(value=value), self.assertRaises(ValueError):
                GUEST.validate_windows_path_text(value)

    @unittest.skipUnless(os.name == "nt", "Windows junction contract")
    def test_rejects_reparse_mount_and_nested_junction(self):
        target = self.root / "outside"
        target.mkdir()
        link = self.workspace / "escape"
        subprocess.run(["cmd.exe", "/c", "mklink", "/J", str(link), str(target)], check=True, capture_output=True)
        self.addCleanup(lambda: os.rmdir(link) if link.exists() else None)
        with self.assertRaises(ValueError):
            self.prepare()
        with self.assertRaises(ValueError):
            GUEST.prepare(link, self.artifacts)

    def test_doctor_never_infers_vm_readiness_from_launcher(self):
        absent = GUEST.readiness({"platform": "Windows", "launcher": None, "featureState": "not_queried_requires_administrator"})
        self.assertEqual(absent["status"], "not_ready")
        self.assertIn("windows_sandbox_launcher_missing", absent["blockers"])
        present = GUEST.readiness({"platform": "Windows", "launcher": r"C:\Windows\System32\WindowsSandbox.exe",
                                   "featureState": "Enabled"})
        self.assertEqual(present["status"], "launch_available_unverified")
        self.assertFalse(present["vmIsolationVerified"])
        self.assertFalse(present["guestControlReady"])

    def test_receipt_is_run_bound_and_not_trusted_as_vm_verification(self):
        prepared = self.prepare()
        manifest = Path(prepared["manifest"])
        self.assertEqual(GUEST.status(manifest)["status"], "prepared_not_launched")
        path = Path(prepared["artifactDirectory"]) / "guest-receipt.json"
        path.write_text(json.dumps({"schemaVersion": 1, "runId": "another-run", "state": "bootstrap_completed"}), encoding="utf-8")
        self.assertEqual(GUEST.status(manifest)["status"], "invalid_guest_receipt")
        path.write_text(json.dumps({"schemaVersion": 1, "runId": prepared["runId"], "state": "bootstrap_completed",
                                    "inputMounted": True, "artifactWritable": True, "guestAccount": "WDAGUtilityAccount"}), encoding="utf-8")
        result = GUEST.status(manifest)
        self.assertEqual(result["status"], "guest_reported")
        self.assertFalse(result["vmIsolationVerified"])
        self.assertFalse(result["guestControlReady"])

    def test_changed_configuration_is_not_accepted(self):
        prepared = self.prepare()
        path = Path(prepared["configuration"]["path"])
        path.write_text(path.read_text(encoding="utf-8").replace("<Networking>Disable", "<Networking>Enable"), encoding="utf-8")
        with self.assertRaises(ValueError):
            GUEST.status(Path(prepared["manifest"]))

    def test_receipt_size_and_array_are_rejected(self):
        prepared = self.prepare()
        path = Path(prepared["artifactDirectory"]) / "guest-receipt.json"
        for payload in ("x" * 65537, "[]"):
            path.write_text(payload, encoding="utf-8")
            self.assertEqual(GUEST.status(Path(prepared["manifest"]))["status"], "invalid_guest_receipt")

    def test_corrupt_manifest_is_refused(self):
        prepared = self.prepare()
        path = Path(prepared["manifest"])
        path.write_text("[]", encoding="utf-8")
        with self.assertRaises(ValueError):
            GUEST.status(path)

    @unittest.skipUnless(os.name == "nt", "Windows junction contract")
    def test_status_rechecks_input_before_manual_launch(self):
        prepared = self.prepare()
        target = self.root / "outside"
        target.mkdir()
        link = self.workspace / "added-later"
        subprocess.run(["cmd.exe", "/c", "mklink", "/J", str(link), str(target)], check=True, capture_output=True)
        self.addCleanup(lambda: os.rmdir(link) if link.exists() else None)
        with self.assertRaises(ValueError):
            GUEST.status(Path(prepared["manifest"]))


if __name__ == "__main__":
    unittest.main()
