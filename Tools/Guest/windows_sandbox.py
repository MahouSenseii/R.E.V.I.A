"""Prepare explicit Windows Sandbox mounts; never install features or infer VM readiness."""
from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import ntpath
import os
import platform
import re
import stat
import sys
import uuid
import xml.etree.ElementTree as ET
from datetime import datetime, timezone
from pathlib import Path


GUEST_INPUT = r"C:\Revia\Input"
GUEST_CONTROL = r"C:\Revia\Control"
GUEST_ARTIFACTS = r"C:\Revia\Artifacts"
MAX_ENTRIES = 100000
MAX_RECEIPT_BYTES = 65536
BOOTSTRAP = Path(__file__).with_name("guest-bootstrap.ps1")


def timestamp() -> str:
    return datetime.now(timezone.utc).isoformat()


def validate_windows_path_text(value: str) -> None:
    drive, tail = ntpath.splitdrive(value)
    if not re.fullmatch(r"[A-Za-z]:", drive) or not tail.startswith(("/", "\\")):
        raise ValueError("Use an absolute local drive path; UNC and device paths are not supported.")
    if any(ord(character) < 32 for character in value) or any(character in value for character in '%<>"|?*'):
        raise ValueError("Paths cannot contain control characters, environment expansion, or Windows wildcard/device syntax.")
    for part in re.split(r"[\\/]", tail):
        if not part:
            continue
        if part in (".", "..") or part != part.strip() or part.endswith(".") or ":" in part:
            raise ValueError("Paths cannot contain traversal, trailing spaces/dots, or alternate data streams.")
        if re.fullmatch(r"CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9]", part.split(".")[0], re.IGNORECASE):
            raise ValueError("Windows device names are not valid mount paths.")


def reject_reparse(path: Path) -> None:
    attributes = path.lstat()
    if stat.S_ISLNK(attributes.st_mode) or getattr(attributes, "st_file_attributes", 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT:
        raise ValueError(f"Reparse points and symbolic links cannot be shared: {path}")


def within(path: Path, root: Path) -> bool:
    return path == root or root in path.parents


def checked_directory(value: Path) -> Path:
    if os.name == "nt":
        validate_windows_path_text(str(value))
    if not value.is_absolute() or not value.is_dir():
        raise ValueError("Mount directories must already exist and use absolute paths.")
    for ancestor in (value, *value.parents):
        reject_reparse(ancestor)
    canonical = value.resolve(strict=True)
    if canonical == Path(canonical.anchor) or canonical == Path.home().resolve():
        raise ValueError("Select a specific workspace/output folder, not a drive or user-profile root.")
    for variable in ("SystemRoot", "ProgramFiles", "ProgramFiles(x86)", "ProgramData"):
        protected = os.environ.get(variable)
        if protected and within(canonical, Path(protected).resolve()):
            raise ValueError("System and application directories cannot be shared as guest workspaces.")
    return canonical


def check_input_tree(directory: Path) -> None:
    pending = [directory]
    count = 0
    while pending:
        with os.scandir(pending.pop()) as children:
            for child in children:
                count += 1
                if count > MAX_ENTRIES:
                    raise ValueError("The selected input exceeds 100,000 entries; select a smaller disposable workspace.")
                reject_reparse(Path(child.path))
                if child.is_dir(follow_symlinks=False):
                    pending.append(Path(child.path))


def write_new(path: Path, content: bytes) -> None:
    with path.open("xb") as stream:
        stream.write(content)
        stream.flush()
        os.fsync(stream.fileno())


def write_json(path: Path, value: dict) -> None:
    write_new(path, (json.dumps(value, indent=2, ensure_ascii=False) + "\n").encode("utf-8"))


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def probe_host() -> dict:
    facts = {"platform": platform.system(), "architecture": platform.machine(), "launcher": None,
             "featureState": "not_queried", "virtualizationFirmware": "unverified"}
    if os.name != "nt":
        return facts
    import winreg
    launcher = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32" / "WindowsSandbox.exe"
    if launcher.is_file():
        facts["launcher"] = str(launcher)
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft\Windows NT\CurrentVersion") as key:
            for field in ("ProductName", "EditionID", "CurrentBuildNumber", "DisplayVersion"):
                try:
                    facts[field] = winreg.QueryValueEx(key, field)[0]
                except OSError:
                    pass
    except OSError:
        facts["editionProbe"] = "unavailable"
    facts["administrator"] = bool(ctypes.windll.shell32.IsUserAnAdmin())
    facts["featureState"] = "not_queried_requires_administrator" if not facts["administrator"] else "not_queried_no_feature_changes"
    return facts


def readiness(facts: dict) -> dict:
    blockers = []
    if facts.get("platform") != "Windows":
        blockers.append("windows_host_required")
    if not facts.get("launcher"):
        blockers.append("windows_sandbox_launcher_missing")
    return {"schemaVersion": 1, "kind": "windows-sandbox-host-readiness", "observedAt": timestamp(), "host": facts,
            "status": "not_ready" if blockers else "launch_available_unverified", "blockers": blockers,
            "vmIsolationVerified": False, "guestControlReady": False,
            "nextStep": "Install or enable Windows Sandbox through an administrator, verify firmware virtualization, and restart if required."
                        if blockers else "Review a prepared .wsb configuration and explicitly launch it; running-guest acceptance remains separate.",
            "limits": ["No feature installation, elevation, reboot, guest launch, capture, input, or process execution was attempted.",
                       "An installed launcher is not proof that virtualization or a guest is running."]}


def prepare(workspace: Path, artifact_root: Path, *, network: bool = False, memory_mb: int = 4096) -> dict:
    if not 2048 <= memory_mb <= 16384:
        raise ValueError("Guest memory must be between 2048 and 16384 MiB.")
    source = checked_directory(workspace)
    output = checked_directory(artifact_root)
    if within(source, output) or within(output, source):
        raise ValueError("Selected input and output roots must be disjoint, with neither containing the other.")
    check_input_tree(source)
    run_id = str(uuid.uuid4())
    bundle = output / run_id
    bundle.mkdir()
    control, artifacts = bundle / "control", bundle / "artifacts"
    control.mkdir()
    artifacts.mkdir()
    script = control / "guest-bootstrap.ps1"
    write_new(script, BOOTSTRAP.read_bytes())
    contract = {"schemaVersion": 1, "runId": run_id, "hostComputerName": platform.node(),
                "inputDirectory": GUEST_INPUT, "artifactDirectory": GUEST_ARTIFACTS, "networkEnabled": network}
    write_json(control / "guest-contract.json", contract)
    config = ET.Element("Configuration")
    for name, value in (("Networking", "Enable" if network else "Disable"), ("vGPU", "Disable"),
                        ("ClipboardRedirection", "Disable"), ("AudioInput", "Disable"), ("VideoInput", "Disable"),
                        ("PrinterRedirection", "Disable"), ("ProtectedClient", "Enable"), ("MemoryInMB", str(memory_mb))):
        ET.SubElement(config, name).text = value
    mounts = ET.SubElement(config, "MappedFolders")
    for host, guest, readonly in ((source, GUEST_INPUT, True), (control, GUEST_CONTROL, True), (artifacts, GUEST_ARTIFACTS, False)):
        mount = ET.SubElement(mounts, "MappedFolder")
        for name, value in (("HostFolder", str(host)), ("SandboxFolder", guest), ("ReadOnly", str(readonly).lower())):
            ET.SubElement(mount, name).text = value
    logon = ET.SubElement(config, "LogonCommand")
    ET.SubElement(logon, "Command").text = (
        r'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe -NoLogo -NoProfile -NonInteractive '
        r'-ExecutionPolicy Bypass -File C:\Revia\Control\guest-bootstrap.ps1')
    ET.indent(config, space="  ")
    configuration = bundle / "environment.wsb"
    write_new(configuration, ET.tostring(config, encoding="utf-8", xml_declaration=True))
    manifest_path = bundle / "launch-manifest.json"
    manifest = {"schemaVersion": 1, "kind": "windows-sandbox-launch-intent", "runId": run_id, "createdAt": timestamp(),
                "manifest": str(manifest_path), "state": "prepared_not_launched", "inputDirectory": str(source),
                "artifactDirectory": str(artifacts), "configuration": {"path": str(configuration), "sha256": digest(configuration)},
                "bootstrapSha256": digest(script), "contractSha256": digest(control / "guest-contract.json"),
                "guestReceipt": str(artifacts / "guest-receipt.json"), "networkEnabled": network,
                "vmIsolationVerified": False, "guestControlReady": False,
                "capabilities": {"capture": False, "input": False, "process": False, "reset": False, "reconnect": False},
                "readiness": readiness(probe_host())}
    write_json(manifest_path, manifest)
    return manifest


def status(manifest_path: Path) -> dict:
    bundle = checked_directory(manifest_path.parent)
    if manifest_path.name != "launch-manifest.json":
        raise ValueError("Select a generated launch-manifest.json.")
    reject_reparse(manifest_path)
    if manifest_path.stat().st_size > MAX_RECEIPT_BYTES:
        raise ValueError("Launch manifest exceeds its size limit.")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if not isinstance(manifest, dict) or manifest.get("schemaVersion") != 1 or manifest.get("kind") != "windows-sandbox-launch-intent":
        raise ValueError("Unsupported launch manifest.")
    configuration, control, artifacts = bundle / "environment.wsb", bundle / "control", bundle / "artifacts"
    source = checked_directory(Path(manifest["inputDirectory"]))
    if within(source, bundle) or within(bundle, source):
        raise ValueError("Selected input and guest output must remain disjoint.")
    check_input_tree(source)
    for directory in (control, artifacts):
        checked_directory(directory)
    if str(configuration) != manifest["configuration"]["path"] or str(artifacts) != manifest["artifactDirectory"]:
        raise ValueError("The manifest paths do not belong to this generated bundle.")
    for path, expected in ((configuration, manifest["configuration"]["sha256"]),
                           (control / "guest-bootstrap.ps1", manifest["bootstrapSha256"]),
                           (control / "guest-contract.json", manifest["contractSha256"])):
        reject_reparse(path)
        if digest(path) != expected:
            raise ValueError("The prepared configuration or bootstrap changed; prepare a fresh run.")
    result = {"schemaVersion": 1, "runId": manifest["runId"], "status": "prepared_not_launched",
              "vmIsolationVerified": False, "guestControlReady": False,
              "limits": ["A guest-authored receipt is untrusted evidence, not independent proof of VM isolation or tool capability."]}
    receipt = artifacts / "guest-receipt.json"
    if receipt.exists():
        reject_reparse(receipt)
        try:
            if receipt.stat().st_size > MAX_RECEIPT_BYTES:
                raise ValueError("Guest receipt exceeds its size limit.")
            reported = json.loads(receipt.read_text(encoding="utf-8-sig"))
            valid = reported.get("schemaVersion") == 1 and reported.get("runId") == manifest["runId"] and \
                reported.get("state") == "bootstrap_completed" and reported.get("inputMounted") is True and \
                reported.get("artifactWritable") is True and reported.get("guestAccount") == "WDAGUtilityAccount"
            result["status"] = "guest_reported" if valid else "invalid_guest_receipt"
        except (ValueError, UnicodeError, AttributeError):
            result["status"] = "invalid_guest_receipt"
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    doctor = commands.add_parser("doctor", help="Read host readiness without changing Windows features.")
    doctor.add_argument("--receipt", type=Path, help="Optional new JSON receipt; never overwritten.")
    create = commands.add_parser("prepare", help="Generate a fresh .wsb and launch intent; does not launch it.")
    create.add_argument("--workspace", type=Path, required=True)
    create.add_argument("--artifact-root", type=Path, required=True)
    create.add_argument("--network", action="store_true", help="Explicitly enable guest networking.")
    create.add_argument("--memory-mb", type=int, default=4096)
    inspect = commands.add_parser("status", help="Inspect run-bound guest evidence without trusting its claims.")
    inspect.add_argument("manifest", type=Path)
    arguments = parser.parse_args()
    try:
        if arguments.command == "doctor":
            result = readiness(probe_host())
            if arguments.receipt:
                checked_directory(arguments.receipt.parent)
                if os.name == "nt":
                    validate_windows_path_text(str(arguments.receipt))
                write_json(arguments.receipt, result)
        elif arguments.command == "prepare":
            result = prepare(arguments.workspace, arguments.artifact_root, network=arguments.network, memory_mb=arguments.memory_mb)
        else:
            result = status(arguments.manifest)
        print(json.dumps(result, indent=2, ensure_ascii=False))
        return 2 if result.get("status") == "not_ready" else 0
    except (ValueError, OSError, KeyError, TypeError) as error:
        print(f"Guest setup refused: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
