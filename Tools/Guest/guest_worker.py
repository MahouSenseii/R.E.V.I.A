"""One request per forced SSH command. No model, identity store, or host access."""
from __future__ import annotations

import hashlib
import contextlib
import ctypes
import json
import os
from pathlib import Path, PurePosixPath
import re
import signal
import stat
import subprocess
import sys
import threading
import time
import uuid

WORKSPACE = Path("/home/revia/workspace")
MAX_TEXT = 262144
MAX_REQUEST = 524288
MAX_OUTPUT = 65536
EXECUTABLES = frozenset(("/usr/bin/python3", "/usr/bin/cmake", "/usr/bin/ctest", "/usr/bin/ninja",
                         "/usr/bin/git", "/usr/bin/node", "/usr/bin/npm", "/usr/bin/g++", "/usr/bin/gcc"))
cancelled = threading.Event()


class RequestError(Exception):
    pass


def missing_executables():
    return sorted(path for path in EXECUTABLES if not os.access(path, os.X_OK))


def request_limits(request):
    limits = {"maxReadBytes": 65536, "maxWriteBytes": 65536, "maxOutputBytes": MAX_OUTPUT,
              "maxEntries": 256, "maxTimeoutMs": 120000, "approvedExecutables": sorted(EXECUTABLES)}
    supplied = request.get("limits", {})
    if not isinstance(supplied, dict) or any(name not in limits for name in supplied):
        raise RequestError("invalid_limits")
    for name, maximum in (("maxReadBytes", MAX_TEXT), ("maxWriteBytes", MAX_TEXT),
                          ("maxOutputBytes", MAX_OUTPUT), ("maxEntries", 512), ("maxTimeoutMs", 120000)):
        value = supplied.get(name, limits[name])
        if isinstance(value, bool) or not isinstance(value, int) or not 1 <= value <= maximum:
            raise RequestError("invalid_limits")
        limits[name] = value
    approved = supplied.get("approvedExecutables", limits["approvedExecutables"])
    if not isinstance(approved, list) or any(not isinstance(item, str) or item not in EXECUTABLES for item in approved):
        raise RequestError("invalid_executable_policy")
    limits["approvedExecutables"] = approved
    return limits


def watch_disconnect(stream):
    # The host keeps stdin open after the single request line. EOF or additional data cancels.
    try:
        # Avoid a daemon holding Python's buffered-stdin lock during interpreter shutdown.
        os.read(stream.fileno(), 1)
    except OSError:
        pass
    finally:
        cancelled.set()


def confined(root: Path, value: str, allow_root: bool = False) -> Path:
    if not isinstance(value, str) or not value or len(value.encode("utf-8")) > 2048 or "\\" in value or any(ord(item) < 32 or ord(item) == 127 for item in value):
        raise RequestError("invalid_guest_path")
    segments = value.split("/")
    if value != "." and any(part in ("", ".", "..") or len(part.encode("utf-8")) > 255 for part in segments):
        raise RequestError("invalid_guest_path")
    parts = PurePosixPath(value)
    if parts.is_absolute() or ".." in parts.parts or ":" in value or (value == "." and not allow_root):
        raise RequestError("path_outside_workspace")
    current = root
    for part in parts.parts:
        current = current / part
        if current.is_symlink():
            raise RequestError("symlink_not_allowed")
    base = root.resolve(strict=True)
    if not current.resolve().is_relative_to(base):
        raise RequestError("path_outside_workspace")
    return current


@contextlib.contextmanager
def anchored_directory(root: Path, relative="."):
    """Keep every POSIX parent open and reject links while walking with openat."""
    path = confined(root, relative, True)
    if os.name != "posix":  # Portable file regression harness; the served helper requires Linux.
        yield path, None
        return
    flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW
    descriptor = os.open(root, flags)
    try:
        for part in PurePosixPath(relative).parts:
            child = os.open(part, flags, dir_fd=descriptor)
            os.close(descriptor)
            descriptor = child
        yield path, descriptor
    finally:
        os.close(descriptor)


def regular_bytes(path, limit: int = MAX_TEXT, directory=None) -> bytes:
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_BINARY", 0)
    descriptor = os.open(path, flags, dir_fd=directory)
    try:
        info = os.fstat(descriptor)
        if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or info.st_size > limit:
            raise RequestError("file_is_not_bounded_regular_text")
        data = os.read(descriptor, limit + 1)
        if len(data) > limit:
            raise RequestError("file_too_large")
        return data
    finally:
        os.close(descriptor)


def enable_subreaper():
    library = ctypes.CDLL(None, use_errno=True)
    if library.prctl(36, 1, 0, 0, 0) != 0:  # PR_SET_CHILD_SUBREAPER: orphaned grandchildren remain ours.
        raise RequestError("process_cleanup_tracking_unavailable")


def process_birth(pid):
    try:
        fields = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()
        return fields[19]  # starttime, including PID-reuse protection.
    except (OSError, IndexError):
        return None


def owned_descendants():
    found = {}
    pending = [os.getpid()]
    while pending:
        parent = pending.pop()
        try:
            tasks = list(Path(f"/proc/{parent}/task").iterdir())
        except FileNotFoundError:
            continue
        for task in tasks:
            try:
                children = (task / "children").read_text().split()
            except FileNotFoundError:
                continue
            for item in children:
                pid = int(item)
                if pid in found or pid == os.getpid():
                    continue
                birth = process_birth(pid)
                if birth is not None:
                    found[pid] = birth
                    pending.append(pid)
                if len(found) > 512:
                    raise RequestError("process_descendant_limit")
    return found


def cleanup_owned(child, tracked):
    deadline = time.monotonic() + 3
    try:
        while True:
            tracked.update(owned_descendants())
            for pid, birth in tracked.items():
                if process_birth(pid) == birth:
                    try:
                        os.kill(pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
            # Reap the direct child through Popen before reaping adopted descendants.
            if child.poll() is not None:
                try:
                    while os.waitpid(-1, os.WNOHANG)[0]:
                        pass
                except ChildProcessError:
                    pass
                if not owned_descendants() and all(process_birth(pid) != birth for pid, birth in tracked.items()):
                    return True
            if time.monotonic() >= deadline:
                return False
            time.sleep(0.02)
    except (OSError, RequestError, ValueError):
        return False


def execute(request: dict, root: Path, limits: dict) -> dict:
    executable = request.get("executable")
    arguments = request.get("arguments", [])
    timeout = request.get("timeoutMs", 30000)
    if executable not in limits["approvedExecutables"] or not isinstance(arguments, list) or len(arguments) > 128 or any(
            not isinstance(arg, str) or "\x00" in arg or len(arg.encode("utf-8")) > 8192 for arg in arguments):
        raise RequestError("executable_or_arguments_not_allowed")
    working_directory = request.get("workingDirectory", ".")
    if not isinstance(working_directory, str):
        raise RequestError("invalid_guest_path")
    if sum(len(item.encode("utf-8")) for item in [executable, working_directory, *arguments]) > 65536:
        raise RequestError("executable_or_arguments_not_allowed")
    if not isinstance(timeout, int) or isinstance(timeout, bool) or not 1 <= timeout <= limits["maxTimeoutMs"]:
        raise RequestError("timeout_out_of_range")
    working = confined(root, request.get("workingDirectory", "."), True)
    if not working.is_dir():
        raise RequestError("working_directory_missing")
    if os.name != "posix":
        raise RequestError("linux_guest_required")
    enable_subreaper()
    environment = {"PATH": "/usr/bin:/bin", "HOME": "/home/revia", "LANG": "C.UTF-8", "LC_ALL": "C.UTF-8"}
    try:
        with anchored_directory(root, request.get("workingDirectory", ".")) as (_, descriptor):
            child = subprocess.Popen([executable, *arguments], cwd=f"/proc/self/fd/{descriptor}", env=environment,
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                start_new_session=True, pass_fds=(descriptor,))
    except OSError:
        raise RequestError("guest_process_start_failed") from None
    collected = [bytearray(), bytearray()]
    total = 0
    truncated = False
    lock = threading.Lock()

    def drain(stream, destination):
        nonlocal total, truncated
        try:
            while chunk := stream.read(4096):
                with lock:
                    remaining = max(0, limits["maxOutputBytes"] - total)
                    destination.extend(chunk[:remaining])
                    total += min(len(chunk), remaining)
                    truncated |= len(chunk) > remaining
        finally:
            stream.close()

    threads = [threading.Thread(target=drain, args=(stream, output), daemon=True)
               for stream, output in zip((child.stdout, child.stderr), collected)]
    for thread in threads:
        thread.start()
    deadline = time.monotonic() + timeout / 1000
    timed_out = False
    parent = os.getppid()
    birth = process_birth(child.pid)
    tracked = {child.pid: birth} if birth is not None else {}
    cleanup_confirmed = False
    monitor_failed = False
    try:
        while child.poll() is None:
            tracked.update(owned_descendants())
            if cancelled.is_set() or os.getppid() != parent or time.monotonic() >= deadline:
                timed_out = time.monotonic() >= deadline
                break
            time.sleep(0.02)
    except (OSError, RequestError, ValueError):
        monitor_failed = True
    finally:
        # Kill descendants even when the direct child exits successfully.
        try:
            os.killpg(child.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        cleanup_confirmed = cleanup_owned(child, tracked) and not monitor_failed
        for thread in threads:
            thread.join(timeout=2)
    was_cancelled = cancelled.is_set() or os.getppid() != parent
    success = child.returncode == 0 and not timed_out and not was_cancelled and cleanup_confirmed
    return {"ok": success, "attempted": True, "succeeded": success, "uncertainEffect": not success,
            "receiptVerified": cleanup_confirmed, "cleanupConfirmed": cleanup_confirmed,
            "code": "cleanup_unverified" if not cleanup_confirmed else "completed" if success else "cancelled" if was_cancelled else "timeout" if timed_out else "guest_process_failed",
            "message": "", "exitCode": child.returncode if child.returncode is not None else -1, "standardOutput": collected[0].decode("utf-8", "replace"),
                        "standardError": collected[1].decode("utf-8", "replace"), "timedOut": timed_out,
                        "cancelled": was_cancelled, "outputTruncated": truncated}


def handle(request: dict, root: Path = WORKSPACE) -> dict:
    limits = request_limits(request)
    if cancelled.is_set():
        raise RequestError("cancelled")
    operation = request.get("operation")
    if operation == "execute":
        return execute(request, root, limits)
    path = confined(root, request.get("path", "."), operation == "list_directory")
    if operation == "list_directory":
        with anchored_directory(root, request.get("path", ".")) as (directory, descriptor):
            entries = []
            with os.scandir(descriptor if descriptor is not None else directory) as iterator:
                for entry in iterator:
                    if len(entries) >= limits["maxEntries"]:
                        raise RequestError("directory_entry_limit")
                    entries.append(entry.name)
            return {"ok": True, "attempted": True, "entries": sorted(entries)}
    parent = str(PurePosixPath(request["path"]).parent)
    with anchored_directory(root, parent) as (directory, descriptor):
        return file_request(request, path, directory, descriptor, limits)


def file_request(request, path, directory, descriptor, limits):
    operation = request["operation"]
    target = path.name if descriptor is not None else path
    if operation == "read_text":
        content = regular_bytes(target, limits["maxReadBytes"], descriptor)
        try:
            text = content.decode("utf-8")
        except UnicodeError:
            raise RequestError("file_is_not_utf8") from None
        return {"ok": True, "attempted": True, "content": text, "digest": hashlib.sha256(content).hexdigest()}
    if operation != "write_text":
        raise RequestError("unsupported_operation")
    text, expected = request.get("content"), request.get("expectedDigest")
    if not isinstance(text, str) or len(text.encode("utf-8")) > limits["maxWriteBytes"]:
        raise RequestError("text_too_large_or_invalid")
    if expected != "missing" and (not isinstance(expected, str) or not re.fullmatch("[a-f0-9]{64}", expected)):
        raise RequestError("expected_digest_required")
    def check_digest():
        try:
            previous = regular_bytes(target, MAX_TEXT, descriptor)
        except FileNotFoundError:
            if expected != "missing":
                raise RequestError("write_conflict") from None
        else:
            if expected == "missing" or hashlib.sha256(previous).hexdigest() != expected:
                raise RequestError("write_conflict")
    check_digest()
    content = text.encode("utf-8")
    # Exclusive temporary creation and atomic replacement preserve the original on I/O failure.
    temporary = ".revia-write-" + str(uuid.uuid4())
    if descriptor is None:
        temporary = directory / temporary
    try:
        file_handle = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | getattr(os, "O_BINARY", 0), 0o600, dir_fd=descriptor)
        with os.fdopen(file_handle, "wb") as stream:
            stream.write(content)
            stream.flush()
            os.fsync(stream.fileno())
        check_digest()
        if cancelled.is_set():
            raise RequestError("cancelled")
        if expected == "missing":
            # Atomic no-replace creation also closes the final check/publish race.
            os.link(temporary, target, src_dir_fd=descriptor, dst_dir_fd=descriptor, follow_symlinks=False)
        else:
            os.replace(temporary, target, src_dir_fd=descriptor, dst_dir_fd=descriptor)
        if descriptor is not None:
            os.fsync(descriptor)
    finally:
        try:
            os.unlink(temporary, dir_fd=descriptor)
        except FileNotFoundError:
            pass
    return {"ok": True, "attempted": True, "succeeded": True, "digest": hashlib.sha256(content).hexdigest()}


def main() -> int:
    for name in ("SIGTERM", "SIGHUP", "SIGINT"):
        if hasattr(signal, name):
            signal.signal(getattr(signal, name), lambda *_: cancelled.set())
    result = {"schemaVersion": 1, "ok": False, "attempted": False, "succeeded": False,
              "uncertainEffect": False, "code": "invalid_request", "message": ""}
    try:
        environment_id = Path("/etc/revia-environment-id").read_text().strip()
        boot_id = Path("/proc/sys/kernel/random/boot_id").read_text().strip()
        result.update(environmentId=environment_id, bootId=boot_id)
        if os.name != "posix":
            raise RequestError("linux_guest_required")
        raw = sys.stdin.buffer.readline(MAX_REQUEST + 1)
        if len(raw) > MAX_REQUEST or not raw.endswith(b"\n"):
            raise RequestError("request_too_large")
        threading.Thread(target=watch_disconnect, args=(sys.stdin.buffer,), daemon=True).start()
        request = json.loads(raw)
        if not isinstance(request, dict) or request.get("schemaVersion") != 1:
            raise RequestError("invalid_schema")
        request_id = request.get("requestId", "")
        if not isinstance(request_id, str) or not re.fullmatch(r"[a-zA-Z0-9_-]{1,80}", request_id):
            raise RequestError("invalid_request_id")
        result.update(requestId=request_id, operation=request.get("operation", ""))
        if request.get("environmentId") != environment_id:
            raise RequestError("environment_mismatch")
        if request.get("operation") == "health":
            missing = missing_executables()
            result.update(ok=True, succeeded=True, code="healthy", ready=Path("/var/lib/revia/bootstrap-ready").is_file() and not missing,
                          missingExecutables=missing,
                          workspace=str(WORKSPACE), executables=sorted(EXECUTABLES), capabilities=["read_text", "write_text", "list_directory", "execute"])
        else:
            if request.get("bootId") != boot_id:
                raise RequestError("stale_boot_epoch")
            # Serialize helpers; code execution remains isolated by the VM, not by this lock.
            import fcntl
            with anchored_directory(WORKSPACE) as (_, descriptor):
                try:
                    fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
                except OSError:
                    raise RequestError("guest_busy") from None
                result.update(handle(request))
            result["succeeded"] = result.get("ok", False)
            if result["ok"]:
                result["code"] = "completed"
    except RequestError as error:
        result["code"] = str(error)
    except (OSError, ValueError, TypeError, UnicodeError):
        result["code"] = "guest_request_failed"
        if result.get("operation") in ("execute", "write_text"):
            result.update(attempted=True, uncertainEffect=True)
    print(json.dumps(result, ensure_ascii=True, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
