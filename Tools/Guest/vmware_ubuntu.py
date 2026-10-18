"""Enrolled VMware Ubuntu workspace. Run --help; no model-authored host commands.

Image pin: Canonical release-20260926 SHA256SUMS, independently checked at authoring.
https://cloud-images.ubuntu.com/releases/noble/release-20260926/SHA256SUMS
VMware commands are the installed vmcli/vmrun/vdiskmanager help contracts.
NoCloud seed: https://docs.cloud-init.io/en/latest/reference/datasources/nocloud.html
"""
from __future__ import annotations

import argparse
import contextlib
import ctypes
import hashlib
import ipaddress
import json
import os
from pathlib import Path
import re
import shutil
import signal
import stat
import subprocess
import sys
import threading
import time
import urllib.parse
import urllib.request
import uuid

IMAGE_NAME = "ubuntu-24.04-server-cloudimg-amd64.vmdk"
IMAGE_URL = "https://cloud-images.ubuntu.com/releases/noble/release-20260926/" + IMAGE_NAME
IMAGE_SHA256 = "58001251111ae414947c62799e5b74c522ecaa6fa4cd654f7846042a0c60cbd1"
MAX_REQUEST = 524288
MAX_RECEIPT = 1048576
STOP = threading.Event()


class BootstrapError(Exception):
    def __init__(self, code, message=""):
        super().__init__(message or code)
        self.code = code


def safe_path(path: Path) -> Path:
    if not path.is_absolute() or path == Path(path.anchor) or path == Path.home():
        raise BootstrapError("invalid_root", "Use an absolute dedicated VM directory.")
    if any(part in ("..", ".") for part in path.parts) or any(character in str(path) for character in "\r\n\x00"):
        raise BootstrapError("invalid_root")
    for item in (path, *path.parents):
        if item.exists() or item.is_symlink():
            info = item.lstat()
            if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
                raise BootstrapError("reparse_path", "VM paths cannot contain links or reparse points.")
    return path


def atomic_json(path: Path, value: dict):
    safe_path(path)
    temporary = path.with_name(path.name + ".new-" + str(uuid.uuid4()))
    try:
        with temporary.open("x", encoding="utf-8") as stream:
            json.dump(value, stream, ensure_ascii=True, separators=(",", ":"))
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def read_json(path: Path, limit=65536) -> dict:
    safe_path(path)
    if path.stat().st_size > limit:
        raise BootstrapError("invalid_state", "The bounded state file is too large.")
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise BootstrapError("invalid_state")
    return value


def manifest_values(owner_id: str, environment_id: str) -> dict:
    return {"schemaVersion": 1, "ownerId": owner_id, "environmentId": environment_id,
            "provider": "vmware", "guest": "ubuntu-24.04-amd64", "vmx": "machine/revia.vmx",
            "imageUrl": IMAGE_URL, "imageSha256": IMAGE_SHA256, "cpuCount": 2,
            "memoryMiB": 2048, "diskGiB": 32, "workspace": "/home/revia/workspace"}


def read_manifest(root: Path, owner_id: str) -> dict:
    safe_path(root)
    manifest = read_json(root / "manifest.json")
    try:
        environment_id = str(uuid.UUID(manifest.get("environmentId", "")))
    except ValueError:
        raise BootstrapError("invalid_manifest") from None
    if manifest != manifest_values(owner_id, environment_id):
        raise BootstrapError("ownership_mismatch", "The VM enrollment does not match this owner or supported configuration.")
    return manifest


def enroll(root: Path, owner_id: str) -> dict:
    safe_path(root)
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,80}", owner_id):
        raise BootstrapError("invalid_owner")
    if root.exists():
        if not (root / "manifest.json").is_file():
            raise BootstrapError("unknown_directory", "Refusing to adopt a directory without a Revia enrollment.")
        return read_manifest(root, owner_id)
    root.mkdir(parents=True, exist_ok=False)
    manifest = manifest_values(owner_id, str(uuid.uuid4()))
    with (root / "manifest.json").open("x", encoding="utf-8") as stream:
        json.dump(manifest, stream, separators=(",", ":"))
        stream.flush()
        os.fsync(stream.fileno())
    return manifest


@contextlib.contextmanager
def exclusive(root: Path):
    path = root / "bootstrap.lock"
    safe_path(path)
    with path.open("a+b") as stream:
        stream.seek(0)
        if os.name == "nt":
            import msvcrt
            if path.stat().st_size == 0:
                stream.write(b"0")
                stream.flush()
            stream.seek(0)
            try:
                msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
            except OSError:
                raise BootstrapError("busy", "Another bootstrap operation owns this VM.") from None
        else:
            import fcntl
            try:
                fcntl.flock(stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            except OSError:
                raise BootstrapError("busy") from None
        try:
            yield
        finally:
            if os.name == "nt":
                stream.seek(0)
                msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)


def command(argv, timeout=60, input_data=None, keep_stdin=False):
    """Bounded output; cancellation stops only this helper command, never a VM tree."""
    creation = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    buffers = [bytearray(), bytearray()]
    overflow = threading.Event()
    with subprocess.Popen([str(part) for part in argv], stdin=subprocess.PIPE if input_data is not None else subprocess.DEVNULL,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, creationflags=creation) as process:
        def drain(stream, output):
            try:
                while data := stream.read(4096):
                    available = max(0, MAX_RECEIPT - len(output))
                    output.extend(data[:available])
                    if len(data) > available:
                        overflow.set()
            finally:
                stream.close()
        readers = [threading.Thread(target=drain, args=pair, daemon=True)
                   for pair in zip((process.stdout, process.stderr), buffers)]
        for reader in readers:
            reader.start()
        try:
            if input_data is not None:
                process.stdin.write(input_data)
                process.stdin.flush()
                if not keep_stdin:
                    process.stdin.close()
            end = time.monotonic() + timeout
            while process.poll() is None:
                if STOP.is_set() or time.monotonic() >= end or overflow.is_set():
                    if process.stdin and not process.stdin.closed:
                        process.stdin.close()  # SSH EOF gives the guest a chance to cancel its process group.
                    try:
                        process.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        process.kill()
                    raise BootstrapError("cancelled" if STOP.is_set() else "command_output_limit" if overflow.is_set() else "command_timeout")
                time.sleep(0.05)
            for reader in readers:
                reader.join(timeout=3)
            if overflow.is_set():
                raise BootstrapError("command_output_limit")
            if process.returncode:
                # Never echo argv or raw stderr: seed/key/guest payloads can contain private data.
                raise BootstrapError("command_failed", f"{Path(argv[0]).name} exited with code {process.returncode}.")
            return buffers[0].decode("utf-8", "replace")
        finally:
            if process.stdin and not process.stdin.closed:
                process.stdin.close()
            if process.poll() is None:
                process.kill()
            process.wait()


def secure_directory(root: Path):
    if os.name != "nt":
        root.chmod(0o700)
        return
    # Set a protected DACL containing only the current Windows identity, before keys exist.
    account = command([str(Path(os.environ["SystemRoot"]) / "System32/whoami.exe"), "/user", "/fo", "csv", "/nh"])
    match = re.search(r"S-1-5-[0-9-]+", account)
    if not match:
        raise BootstrapError("identity_lookup_failed")
    descriptor = ctypes.c_void_p()
    advapi = ctypes.WinDLL("advapi32", use_last_error=True)
    convert = advapi.ConvertStringSecurityDescriptorToSecurityDescriptorW
    convert.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.POINTER(ctypes.c_void_p), ctypes.c_void_p]
    convert.restype = ctypes.c_int
    apply = advapi.SetFileSecurityW
    apply.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_void_p]
    apply.restype = ctypes.c_int
    if not convert("D:P(A;OICI;FA;;;" + match.group() + ")", 1, ctypes.byref(descriptor), None):
        raise BootstrapError("private_acl_failed")
    try:
        if not apply(str(root), 0x80000004, descriptor):  # PROTECTED_DACL_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION
            raise BootstrapError("private_acl_failed")
    finally:
        release = ctypes.WinDLL("kernel32", use_last_error=True).LocalFree
        release.argtypes = [ctypes.c_void_p]
        release.restype = ctypes.c_void_p
        release(descriptor)


def tools():
    if os.name != "nt":
        raise BootstrapError("windows_host_required")
    base = Path(os.environ.get("ProgramFiles", "C:/Program Files")) / "VMware/VMware Workstation"
    system = Path(os.environ.get("SystemRoot", "C:/Windows")) / "System32/OpenSSH"
    result = {"vmrun": base / "vmrun.exe", "vmcli": base / "vmcli.exe",
              "disk": base / "vmware-vdiskmanager.exe", "iso": base / "mkisofs.exe",
              "ssh": system / "ssh.exe", "keygen": system / "ssh-keygen.exe"}
    for name, path in result.items():
        safe_path(path)
        if not path.is_file():
            raise BootstrapError("missing_tool", f"Required installed tool unavailable: {name}.")
    return result


def check_download_url(url):
    parsed = urllib.parse.urlparse(url)
    if parsed.scheme != "https" or parsed.hostname != "cloud-images.ubuntu.com" or parsed.username or parsed.password or parsed.port:
        raise BootstrapError("untrusted_image_url")


class CanonicalRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, message, headers, new_url):
        check_download_url(new_url)
        return super().redirect_request(request, fp, code, message, headers, new_url)


def file_digest(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while chunk := stream.read(1024 * 1024):
            if STOP.is_set():
                raise BootstrapError("cancelled")
            digest.update(chunk)
    return digest.hexdigest()


def download_verified(url, target: Path, expected_hash, opener=None):
    check_download_url(url)
    safe_path(target)
    if target.exists():
        if file_digest(target) != expected_hash:
            raise BootstrapError("image_hash_mismatch", "The cached image does not match the reviewed image pin.")
        return
    temporary = target.with_name(target.name + ".partial")
    safe_path(temporary)
    try:
        digest = hashlib.sha256()
        count = 0
        end = time.monotonic() + 1800
        with (opener or (lambda value: urllib.request.build_opener(CanonicalRedirect()).open(value, timeout=30)))(url) as response:
            check_download_url(response.geturl())
            with temporary.open("wb") as stream:
                while data := response.read(1024 * 1024):
                    if STOP.is_set() or time.monotonic() > end:
                        raise BootstrapError("cancelled" if STOP.is_set() else "download_timeout")
                    count += len(data)
                    if count > 2 * 1024 ** 3:
                        raise BootstrapError("image_size_limit")
                    digest.update(data)
                    stream.write(data)
                stream.flush()
                os.fsync(stream.fileno())
        if digest.hexdigest() != expected_hash:
            raise BootstrapError("image_hash_mismatch")
        os.replace(temporary, target)
    finally:
        temporary.unlink(missing_ok=True)


def cloud_config(manifest, public_key, host_private=None, host_public=None):
    """The guest's first-boot configuration.

    The guest's SSH host key pair is generated on the host so its public half can be pinned in
    known_hosts before the guest ever answers; cloud-init installs it on first boot. That makes the
    private half part of the seed. It is deleted from the host's key folder as soon as the seed is
    built, and the seed is rebuilt without it (host_private=None) once the guest has booted with it.
    """
    helper = Path(__file__).with_name("guest_worker.py").read_text(encoding="utf-8")
    forced_key = 'restrict,command="/usr/bin/python3 -I -B /opt/revia/guest_worker.py" ' + public_key.strip()
    # JSON scalars are valid YAML scalars, so names, keys and program text never become YAML instructions.
    def scalar(value):
        return json.dumps(value, ensure_ascii=True)
    host_key = [] if host_private is None else [
        "ssh_keys:", "  ed25519_private: " + scalar(host_private), "  ed25519_public: " + scalar(host_public.strip())]
    return "\n".join([
        "#cloud-config", "ssh_pwauth: false", "disable_root: true", "ssh_deletekeys: false", *host_key,
        "users:", "  - name: revia", "    lock_passwd: true", "    shell: /bin/sh", "    ssh_authorized_keys:",
        "      - " + scalar(forced_key), "package_update: true",
        "packages: [open-vm-tools, openssh-server, build-essential, cmake, ninja-build, git, python3-pytest, python3-venv, nodejs, npm]",
        "write_files:", "  - path: /etc/revia-environment-id", "    permissions: '0644'",
        "    content: " + scalar(manifest["environmentId"] + "\n"),
        "  - path: /opt/revia/guest_worker.py", "    permissions: '0755'", "    content: " + scalar(helper),
        "  - path: /etc/ssh/sshd_config.d/20-revia.conf", "    permissions: '0644'",
        "    content: " + scalar("PasswordAuthentication no\nKbdInteractiveAuthentication no\nPermitRootLogin no\nAllowUsers revia\nAllowTcpForwarding no\nX11Forwarding no\nPermitTunnel no\nPermitUserEnvironment no\nPermitUserRC no\nForceCommand /usr/bin/python3 -I -B /opt/revia/guest_worker.py\n"),
        "runcmd:", "  - [install, -d, -o, revia, -g, revia, -m, '0700', /home/revia/workspace]",
        "  - [install, -d, -m, '0755', /var/lib/revia]",
        "  - [systemctl, enable, --now, open-vm-tools]",
        "  - [systemctl, restart, ssh]",
        "  - [sh, -c, 'systemctl is-active --quiet open-vm-tools && test -x /usr/bin/cmake && test -x /usr/bin/ninja && test -x /usr/bin/node && touch /var/lib/revia/bootstrap-ready']", ""])


def write_seed(root, manifest, installed, config):
    """Builds seed.iso from config; the plaintext copy is removed once the image exists."""
    seed = safe_path(root / "seed")
    seed.mkdir(exist_ok=True)
    (seed / "user-data").write_text(config, encoding="utf-8", newline="\n")
    (seed / "meta-data").write_text("instance-id: " + manifest["environmentId"] + "\nlocal-hostname: revia-workspace\n", encoding="utf-8", newline="\n")
    try:
        command([installed["iso"], "-quiet", "-output", root / "seed.new.iso", "-volid", "CIDATA", "-joliet", "-rock", seed])
        os.replace(root / "seed.new.iso", root / "seed.iso")
    finally:
        (seed / "user-data").unlink(missing_ok=True)


def scrub_seed(root, manifest, installed):
    """Rebuilds the seed without the guest's SSH host private key once the guest has booted with it.

    Only while the VM is stopped (Workstation holds the attached image open). cloud-init has already
    run its once-per-instance modules for this instance-id, so the rebuilt seed changes nothing in the
    guest; it only stops the host keeping a copy of the guest's private host key.
    """
    if not recorded(root, "guest-ready.json", manifest) or recorded(root, "seed-scrubbed.json", manifest):
        return
    try:
        write_seed(root, manifest, installed, cloud_config(manifest, (root / "keys/client.pub").read_text()))
    except (BootstrapError, OSError, KeyError):
        # Not a reason to fail a start or a stop: the old seed stays and this is retried
        # the next time the VM is stopped.
        return
    record(root, "seed-scrubbed.json", manifest)


def vm_entries(root, manifest):
    return {"memsize": "2048", "numvcpus": "2", "firmware": "efi", "scsi0.present": "TRUE",
            "scsi0.virtualDev": "lsilogic", "scsi0:0.present": "TRUE", "scsi0:0.fileName": "workspace.vmdk",
            "sata0.present": "TRUE", "sata0:0.present": "TRUE", "sata0:0.deviceType": "cdrom-image",
            "sata0:0.fileName": str(root / "seed.iso"), "sata0:0.startConnected": "TRUE",
            "ethernet0.present": "TRUE", "ethernet0.virtualDev": "vmxnet3", "ethernet0.connectionType": "nat",
            "ethernet0.startConnected": "TRUE", "ethernet0.addressType": "generated",
            "isolation.tools.copy.disable": "TRUE", "isolation.tools.paste.disable": "TRUE",
            "isolation.tools.dnd.disable": "TRUE", "isolation.tools.hgfs.disable": "TRUE",
            "sharedFolder.maxNum": "0", "sound.present": "FALSE", "usb.present": "FALSE",
            "guestinfo.revia.environmentId": manifest["environmentId"]}


def validate_vmx(root, manifest):
    path = safe_path(root / manifest["vmx"])
    if not path.is_file():
        raise BootstrapError("vm_not_provisioned")
    content = path.read_text(encoding="utf-8")
    if len(content) > 262144:
        raise BootstrapError("invalid_vm_configuration")
    parsed = {}
    for line in content.splitlines():
        match = re.fullmatch(r'\s*([^#=]+?)\s*=\s*"(.*)"\s*', line)
        if match:
            name, value = match.groups()
            name = name.lower()
            if name in parsed:
                raise BootstrapError("invalid_vm_configuration")
            parsed[name] = value
    for key, value in vm_entries(root, manifest).items():
        if parsed.get(key.lower()) != value:
            raise BootstrapError("vm_configuration_changed", "The enrolled VM configuration changed; refusing to start it.")
    for key, value in parsed.items():
        if ".hostpath" in key or (key.startswith("sharedfolder") and key != "sharedfolder.maxnum"):
            raise BootstrapError("host_share_present")
        slot = re.match(r"^(?:scsi|sata|ide|nvme)[0-9]+:[0-9]+\.", key)
        if slot and not key.startswith(("scsi0:0.", "sata0:0.")):
            raise BootstrapError("unowned_vm_device")
        adapter = re.match(r"^ethernet([0-9]+)\.", key)
        if adapter and adapter.group(1) != "0":
            raise BootstrapError("unowned_vm_device")
        if re.match(r"^(?:serial|parallel|floppy|pcipassthru)[0-9]+\.", key):
            raise BootstrapError("host_device_present")
        if key == "scsi0:0.devicetype" and value.lower() != "disk":
            raise BootstrapError("host_device_present")
        if key.endswith((".filename", ".hostpath", ".redo")) or key in ("nvram", "extendedconfigfile", "workingdir", "snapshotdirectory", "checkpoint.vmstate", "sched.swap.derivedname"):
            if not value:
                continue
            artifact = Path(value)
            if not artifact.is_absolute():
                artifact = root / "machine" / artifact
            artifact = safe_path(artifact)
            if not artifact.resolve().is_relative_to(root.resolve()):
                raise BootstrapError("unowned_vm_artifact")
    return path


def ssh_command(root, manifest, address, ssh):
    try:
        ip = ipaddress.ip_address(address)
        if ip.version != 4 or ip.is_loopback or ip.is_unspecified or not ip.is_private:
            raise ValueError()
    except ValueError:
        raise BootstrapError("invalid_guest_address") from None
    safe_path(root / "keys/client")
    safe_path(root / "keys/known_hosts")
    return [str(ssh), "-F", "NUL" if os.name == "nt" else "/dev/null", "-T", "-i", str(root / "keys/client"),
            "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=yes", "-o", "IdentitiesOnly=yes",
            "-o", "IdentityAgent=none", "-o", "PasswordAuthentication=no", "-o", "HostKeyAlgorithms=ssh-ed25519",
            "-o", "ForwardAgent=no", "-o", "ClearAllForwardings=yes", "-o", "ConnectTimeout=5",
            "-o", "ServerAliveInterval=5", "-o", "ServerAliveCountMax=2", "-o", "HostKeyAlias=" + manifest["environmentId"],
            "-o", "UserKnownHostsFile=" + str(root / "keys/known_hosts"), "-o", "GlobalKnownHostsFile=NUL" if os.name == "nt" else "GlobalKnownHostsFile=/dev/null",
            "revia@" + str(ip), "/usr/bin/python3", "-I", "-B", "/opt/revia/guest_worker.py"]


def valid_health(value, manifest):
    try:
        return value.get("schemaVersion") == 1 and value.get("ok") is True and value.get("ready") is True and \
            value.get("environmentId") == manifest["environmentId"] and str(uuid.UUID(value.get("bootId", ""))) == value["bootId"]
    except (ValueError, TypeError, AttributeError):
        return False


def running(vmx, installed):
    lines = command([installed["vmrun"], "-T", "ws", "list"]).splitlines()
    return any(os.path.normcase(line.strip()) == os.path.normcase(str(vmx)) for line in lines[1:])


def guest_request(root, manifest, installed, request):
    if request.get("schemaVersion") != 1 or request.get("environmentId") != manifest["environmentId"] or \
            not isinstance(request.get("requestId"), str) or not re.fullmatch(r"[A-Za-z0-9_-]{1,80}", request["requestId"]):
        raise BootstrapError("invalid_request")
    timeout = request.get("timeoutMs", 30000)
    if isinstance(timeout, bool) or not isinstance(timeout, int) or not 1 <= timeout <= 120000:
        raise BootstrapError("invalid_timeout")
    if request.get("operation") not in ("health", "read_text", "write_text", "list_directory", "execute"):
        raise BootstrapError("unsupported_operation")
    vmx = validate_vmx(root, manifest)
    if not running(vmx, installed):
        raise BootstrapError("vm_stopped")
    # Workstation can report Tools unavailable until its first waiting IP query.
    address = command([installed["vmrun"], "-T", "ws", "getGuestIPAddress", str(vmx), "-wait"], 15).strip()
    payload = json.dumps(request, ensure_ascii=True, separators=(",", ":")).encode() + b"\n"
    if len(payload) > MAX_REQUEST:
        raise BootstrapError("request_too_large")
    argv = ssh_command(root, manifest, address, installed["ssh"])
    try:
        response = command(argv, min(130, max(10, timeout / 1000 + 10)), payload, keep_stdin=True)
        try:
            value = json.loads(response)
        except ValueError:
            raise BootstrapError("invalid_guest_receipt") from None
        if not isinstance(value, dict) or value.get("schemaVersion") != 1 or value.get("environmentId") != manifest["environmentId"] or value.get("requestId") != request["requestId"]:
            raise BootstrapError("invalid_guest_receipt")
        if request["operation"] != "health" and value.get("bootId") != request.get("bootId"):
            raise BootstrapError("stale_boot_epoch")
        value["receiptVerified"] = value.get("receiptVerified", True) is True
        return value
    except (BootstrapError, OSError) as error:
        # Once sent, a missing receipt cannot establish whether code or a write took effect.
        code = error.code if isinstance(error, BootstrapError) else "transport_failed"
        return {"schemaVersion": 1, "ok": False, "attempted": True, "succeeded": False,
                "uncertainEffect": request["operation"] in ("execute", "write_text"), "receiptVerified": False,
                "environmentId": manifest["environmentId"], "bootId": request.get("bootId", ""),
                "requestId": request["requestId"], "code": code, "message": "Guest receipt unavailable; the operation was not retried.",
                "cancelled": code == "cancelled", "timedOut": code == "command_timeout"}


def recorded(root, name, manifest):
    path = root / name
    if not path.exists():
        return False
    if read_json(path) != {"schemaVersion": 1, "environmentId": manifest["environmentId"]}:
        raise BootstrapError("artifact_ownership_mismatch")
    return True


def record(root, name, manifest):
    atomic_json(root / name, {"schemaVersion": 1, "environmentId": manifest["environmentId"]})


def status(root, owner_id, installed):
    if not root.exists():
        return {"state": "not_enrolled", "code": "not_enrolled", "guestReady": False}
    manifest = read_manifest(root, owner_id)
    result = {"environmentId": manifest["environmentId"], "vmx": str(root / manifest["vmx"]), "guestReady": False}
    if not recorded(root, "provisioned.json", manifest):
        return {**result, "state": "provisioning", "code": "provisioning_incomplete"}
    vmx = validate_vmx(root, manifest)
    if not running(vmx, installed):
        return {**result, "state": "stopped", "code": "vm_stopped"}
    try:
        health = guest_request(root, manifest, installed, {"schemaVersion": 1, "requestId": str(uuid.uuid4()),
            "environmentId": manifest["environmentId"], "operation": "health"})
    except BootstrapError as error:
        return {**result, "state": "booting", "code": error.code}
    if valid_health(health, manifest) and not recorded(root, "guest-ready.json", manifest):
        # The guest answered with the pinned host key, so it has installed it; the seed's
        # copy can go the next time the VM is stopped.
        record(root, "guest-ready.json", manifest)
    return {**result, "state": "ready" if valid_health(health, manifest) else "bootstrapping",
            "code": "ready" if valid_health(health, manifest) else "guest_bootstrap_pending",
            "guestReady": valid_health(health, manifest), "bootId": health.get("bootId", ""), "health": health}


def available_memory():
    if os.name != "nt":
        return os.sysconf("SC_AVPHYS_PAGES") * os.sysconf("SC_PAGE_SIZE")
    class MemoryStatus(ctypes.Structure):
        _fields_ = [("length", ctypes.c_uint32), ("load", ctypes.c_uint32),
                    ("totalPhysical", ctypes.c_uint64), ("availablePhysical", ctypes.c_uint64),
                    ("totalPage", ctypes.c_uint64), ("availablePage", ctypes.c_uint64),
                    ("totalVirtual", ctypes.c_uint64), ("availableVirtual", ctypes.c_uint64),
                    ("availableExtended", ctypes.c_uint64)]
    value = MemoryStatus()
    value.length = ctypes.sizeof(value)
    if not ctypes.WinDLL("kernel32", use_last_error=True).GlobalMemoryStatusEx(ctypes.byref(value)):
        raise BootstrapError("memory_probe_failed")
    return value.availablePhysical


def check_start_resources(root):
    if available_memory() < 3 * 1024 ** 3:
        raise BootstrapError("insufficient_memory", "VM startup deferred: at least 3 GiB available host memory is required for this 2 GiB guest.")
    if shutil.disk_usage(root).free < 6 * 1024 ** 3:
        raise BootstrapError("insufficient_disk", "VM startup deferred: at least 6 GiB free host disk space is required.")


def stop_guest(root, owner_id, installed):
    if not root.exists():
        return {"ok": True, "state": "stopped", "code": "not_enrolled"}
    manifest = read_manifest(root, owner_id)
    with exclusive(root):
        vmx = safe_path(root / manifest["vmx"])
        if not vmx.exists():
            return {"ok": True, "state": "stopped", "code": "partial_setup_stopped"}
        if not recorded(root, "vm-created.json", manifest):
            raise BootstrapError("unrecorded_vm")
        if running(vmx, installed):
            command([installed["vmrun"], "-T", "ws", "stop", vmx, "soft"], 120)
        if running(vmx, installed):
            raise BootstrapError("vm_still_running")
        scrub_seed(root, manifest, installed)
        return {"ok": True, "state": "stopped", "code": "stopped"}


def ensure(root, owner_id, installed, wait_seconds=0, start_vm=True):
    manifest = enroll(root, owner_id)
    with exclusive(root):
        secure_directory(root)
        for name in ("keys", "seed", "cache", "machine"):
            safe_path(root / name).mkdir(exist_ok=True)
        vmx = root / manifest["vmx"]
        if not recorded(root, "provisioned.json", manifest):
            if vmx.exists() and not recorded(root, "vm-created.json", manifest):
                raise BootstrapError("unrecorded_vm", "An unrecorded VMX exists in the enrollment; preserving it without modification.")
            if vmx.exists() and running(vmx, installed):
                raise BootstrapError("incomplete_vm_running", "An incomplete VM is running; stop it before resuming setup.")
            if shutil.disk_usage(root).free < 12 * 1024 ** 3:
                raise BootstrapError("insufficient_disk", "Provisioning requires at least 12 GiB currently free; the sparse guest disk may grow to 32 GiB.")
            atomic_json(root / "progress.json", {"stage": "downloading_image"})
            cached = root / "cache" / IMAGE_NAME
            download_verified(IMAGE_URL, cached, IMAGE_SHA256)
            # The guest host key's private half is deleted once it is in the seed, so an
            # interrupted setup that finds only its public half makes a fresh pair. The VM
            # has not booted before provisioned.json exists, so nothing has pinned the old one.
            host_key = safe_path(root / "keys/host")
            if not host_key.exists() and host_key.with_suffix(".pub").exists():
                host_key.with_suffix(".pub").unlink()
            for key in ("client", "host"):
                path = root / "keys" / key
                if not path.exists() and not path.with_suffix(".pub").exists():
                    command([installed["keygen"], "-q", "-t", "ed25519", "-N", "", "-C", "revia-" + key, "-f", path])
                if not path.is_file() or not path.with_suffix(".pub").is_file():
                    raise BootstrapError("incomplete_key_pair", "A key operation was interrupted; preserve the enrollment for repair.")
            host_public = (root / "keys/host.pub").read_text().strip()
            (root / "keys/known_hosts").write_text(manifest["environmentId"] + " " + host_public + "\n", encoding="utf-8")
            config = cloud_config(manifest, (root / "keys/client.pub").read_text(), host_key.read_text(), host_public)
            atomic_json(root / "progress.json", {"stage": "creating_seed"})
            write_seed(root, manifest, installed, config)
            # Only the pinned public half is needed from here on.
            host_key.unlink()
            disk = root / "machine/workspace.vmdk"
            safe_path(disk)
            if disk.exists() and not recorded(root, "disk-created.json", manifest):
                raise BootstrapError("unrecorded_disk", "An unrecorded disk exists; preserving it without modification.")
            atomic_json(root / "progress.json", {"stage": "creating_disk"})
            if not disk.exists():
                temporary = root / "machine/workspace.build.vmdk"
                safe_path(temporary).unlink(missing_ok=True)
                command([installed["disk"], "-r", cached, "-t", "0", temporary], 600)
                command([installed["disk"], "-x", "32GB", temporary], 600)
                command([installed["disk"], "-e", temporary], 60)
                # VMware updates the VMDK's internal extent reference during rename.
                command([installed["disk"], "-n", temporary, disk], 60)
                record(root, "disk-created.json", manifest)
            else:
                command([installed["disk"], "-e", disk], 60)
            atomic_json(root / "progress.json", {"stage": "creating_vm"})
            if not vmx.exists():
                command([installed["vmcli"], "VM", "Create", "--name", "revia", "--dirpath", root / "machine", "--guesttype", "ubuntu-64"])
                if vmx.is_file():
                    record(root, "vm-created.json", manifest)
            if not vmx.is_file():
                raise BootstrapError("vm_layout_unexpected", "VMware did not create the expected enrolled VMX path.")
            for key, value in vm_entries(root, manifest).items():
                command([installed["vmcli"], str(vmx), "ConfigParams", "SetEntry", key, value])
            validate_vmx(root, manifest)
            record(root, "provisioned.json", manifest)
        validate_vmx(root, manifest)
        if not start_vm:
            return {"state": "provisioned", "code": "provisioned", "guestReady": False,
                    "environmentId": manifest["environmentId"], "vmx": str(vmx)}
        if not running(vmx, installed):
            scrub_seed(root, manifest, installed)
            check_start_resources(root)
            atomic_json(root / "progress.json", {"stage": "starting_vm"})
            command([installed["vmrun"], "-T", "ws", "start", vmx, "nogui"], 120)
        deadline = time.monotonic() + wait_seconds
        while True:
            current = status(root, owner_id, installed)
            atomic_json(root / "progress.json", {"stage": current["state"], "code": current["code"]})
            if current["guestReady"] or time.monotonic() >= deadline:
                return current
            if STOP.wait(2):
                raise BootstrapError("cancelled")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("doctor", "status", "provision", "ensure", "start", "stop", "request"))
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--owner-id", required=True)
    parser.add_argument("--wait-seconds", type=int, default=0)
    parser.add_argument("--request-file", type=Path)
    options = parser.parse_args()
    for name in ("SIGINT", "SIGTERM"):
        signal.signal(getattr(signal, name), lambda *_: STOP.set())
    result = {"schemaVersion": 1, "ok": False, "guestReady": False, "state": "blocked"}
    try:
        safe_path(options.root)
        if not 0 <= options.wait_seconds <= 1800:
            raise BootstrapError("invalid_timeout")
        installed = tools()
        if options.command == "doctor":
            result.update(ok=True, state="provider_available", code="provider_available", tools={key: str(value) for key, value in installed.items()})
        elif options.command == "status":
            result.update(status(options.root, options.owner_id, installed), ok=True)
        elif options.command in ("provision", "ensure", "start"):
            result.update(ensure(options.root, options.owner_id, installed, options.wait_seconds, options.command != "provision"), ok=True)
        elif options.command == "stop":
            result.update(stop_guest(options.root, options.owner_id, installed))
        else:
            manifest = read_manifest(options.root, options.owner_id)
            if not options.request_file:
                raise BootstrapError("request_file_required")
            request = read_json(options.request_file, MAX_REQUEST)
            if request.get("environmentId") != manifest["environmentId"]:
                raise BootstrapError("environment_mismatch")
            result = guest_request(options.root, manifest, installed, request)
        print(json.dumps(result, ensure_ascii=True, separators=(",", ":")))
        # A valid negative operation receipt is still a successfully delivered protocol response.
        return 0 if options.command == "request" or result.get("ok") else 2
    except BootstrapError as error:
        result.update(code=error.code, message=str(error)[:512])
    except (OSError, ValueError, TypeError, KeyError, RuntimeError):
        result.update(code="bootstrap_failed", message="The VM operation failed; no readiness is inferred.")
    print(json.dumps(result, ensure_ascii=True, separators=(",", ":")))
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
