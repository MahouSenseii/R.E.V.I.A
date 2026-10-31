"""Install one pinned Hugging Face snapshot for optional offline training or speculation.

Config/training_assets.json names every file with its exact size and SHA-256. Nothing else is
downloaded, an existing file is never replaced, and an interrupted file resumes. The finished
directory gets the revia-install-manifest.json that Tools/revia_training_worker.py verifies,
and a receipt under _revia_download. Model assets stay outside OneDrive, whose reparse points
broke long-lived private video scratch.

    python -I -B Tools/install_training_assets.py --asset qwen3.5-9b-base --destination D:/ReviaModels
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import http.client
import json
import os
from pathlib import Path
import platform
import re
import shutil
import stat
import sys
import time
import urllib.error
import urllib.request


ASSETS = Path(__file__).resolve().parents[1] / "Config" / "training_assets.json"
INSTALL_MANIFEST = "revia-install-manifest.json"
CHUNK = 8 * 1024 * 1024
MARGIN = 1024 ** 3


def require(condition, message):
    if not condition:
        raise ValueError(message)


def plain(path):
    path = Path(path)
    require(not str(path).startswith(("\\\\", "//")), "Network paths are not model asset destinations.")
    for part in (path, *path.parents):
        if part.exists():
            information = part.lstat()
            require(not stat.S_ISLNK(information.st_mode) and not getattr(information, "st_file_attributes", 0) & 0x400,
                    "Links and reparse points are not model asset destinations.")
    return path


def outside_onedrive(path):
    resolved = Path(path).resolve()
    for variable in ("OneDrive", "OneDriveConsumer", "OneDriveCommercial"):
        root = os.environ.get(variable)
        if root and (resolved == Path(root).resolve() or Path(root).resolve() in resolved.parents):
            raise ValueError("Model assets stay outside OneDrive; choose a local folder such as D:/ReviaModels.")


def load_assets(path=ASSETS):
    document = json.loads(Path(path).read_text(encoding="utf-8"))
    require(document.get("schemaVersion") == 1 and isinstance(document.get("snapshots"), list), "Unsupported training asset list.")
    seen = set()
    for snapshot in document["snapshots"]:
        require(re.fullmatch(r"[a-z0-9.-]{1,64}", str(snapshot.get("id"))) and snapshot["id"] not in seen, "Invalid or duplicate asset id.")
        seen.add(snapshot["id"])
        require(re.fullmatch(r"[A-Za-z0-9_.-]{1,96}/[A-Za-z0-9_.-]{1,96}", str(snapshot.get("repository"))) and
                re.fullmatch(r"[0-9a-f]{40}", str(snapshot.get("revision"))), f"{snapshot['id']} needs a repository and pinned revision.")
        files = snapshot.get("files")
        require(isinstance(files, list) and files, f"{snapshot['id']} lists no files.")
        names = [entry.get("path") for entry in files]
        require(len(set(names)) == len(names), f"{snapshot['id']} lists a file twice.")
        for entry in files:
            require(re.fullmatch(r"[A-Za-z0-9_.-]{1,128}", str(entry.get("path"))) and not entry["path"].startswith(".") and
                    type(entry.get("bytes")) is int and entry["bytes"] > 0 and re.fullmatch(r"[0-9a-f]{64}", str(entry.get("sha256"))),
                    f"{snapshot['id']} has a file without a plain name, size and SHA-256.")
    return {snapshot["id"]: snapshot for snapshot in document["snapshots"]}


def install_manifest(snapshot):
    """Deterministic, so a job can pin its digest before the download happens."""
    files = {entry["path"]: {"bytes": entry["bytes"], "sha256": entry["sha256"]} for entry in snapshot["files"]}
    value = {"repository": snapshot["repository"], "revision": snapshot["revision"], "files": files}
    return (json.dumps(value, indent=1, sort_keys=True) + "\n").encode("utf-8")


def target_directory(snapshot, destination):
    return Path(destination) / f"{snapshot['repository'].split('/')[1]}-{snapshot['revision'][:7]}"


def file_digest(path, limit=None):
    value = hashlib.sha256()
    with Path(path).open("rb") as stream:
        remaining = limit
        while remaining is None or remaining > 0:
            block = stream.read(CHUNK if remaining is None else min(CHUNK, remaining))
            if not block:
                break
            value.update(block)
            if remaining is not None:
                remaining -= len(block)
    return value


def fetch(url, partial, entry, timeout):
    """Appends the missing bytes of one file to partial and returns how many arrived."""
    have = partial.stat().st_size if partial.exists() else 0
    require(have <= entry["bytes"], f"{entry['path']}.partial is larger than the pinned file; remove it and retry.")
    request = urllib.request.Request(url, headers={"User-Agent": "revia-training-assets/1"})
    if have:
        request.add_header("Range", f"bytes={have}-")
    arrived = 0
    with urllib.request.urlopen(request, timeout=timeout) as response:
        if have and response.status != 206:
            # The server ignored the range; start over rather than append a second copy.
            have = 0
            partial.unlink()
        with partial.open("ab") as output:
            while True:
                block = response.read(CHUNK)
                if not block:
                    break
                output.write(block)
                arrived += len(block)
                require(have + arrived <= entry["bytes"], f"{entry['path']} is larger than its pinned size.")
    return arrived


def install(snapshot, destination, endpoint="https://huggingface.co", attempts=4, timeout=60, report=print):
    destination = plain(Path(destination).absolute())
    outside_onedrive(destination)
    target = plain(target_directory(snapshot, destination))
    manifest_bytes = install_manifest(snapshot)
    manifest_path = target / INSTALL_MANIFEST
    if manifest_path.exists():
        require(manifest_path.read_bytes() == manifest_bytes, f"{manifest_path} differs from the pinned snapshot; inspect it.")
        for entry in snapshot["files"]:
            require((target / entry["path"]).stat().st_size == entry["bytes"], f"Installed {entry['path']} has the wrong size.")
        report(json.dumps({"state": "installed", "asset": snapshot["id"], "directory": str(target)}))
        return {"state": "installed", "directory": str(target), "installManifestSha256": hashlib.sha256(manifest_bytes).hexdigest()}
    target.mkdir(parents=True, exist_ok=True)
    missing = [entry for entry in snapshot["files"] if not (target / entry["path"]).exists()]
    needed = sum(entry["bytes"] - ((target / f"{entry['path']}.partial").stat().st_size
                                   if (target / f"{entry['path']}.partial").exists() else 0) for entry in missing)
    require(shutil.disk_usage(target).free >= needed + MARGIN, f"{needed + MARGIN} free bytes are needed at {target}.")
    started = datetime.now(timezone.utc)
    receipts = []
    for entry in snapshot["files"]:
        final = target / entry["path"]
        partial = target / f"{entry['path']}.partial"
        if final.exists():
            require(final.stat().st_size == entry["bytes"] and file_digest(final).hexdigest() == entry["sha256"],
                    f"{final} already exists with different content; it is not replaced.")
            receipts.append({"path": entry["path"], "downloadedBytes": 0, "seconds": 0.0})
            continue
        url = f"{endpoint}/{snapshot['repository']}/resolve/{snapshot['revision']}/{entry['path']}"
        began, downloaded = time.monotonic(), 0
        # A complete partial left by an interrupted earlier run only needs verifying.
        for attempt in range(1, attempts + 1):
            if partial.exists() and partial.stat().st_size == entry["bytes"]:
                break
            try:
                downloaded += fetch(url, partial, entry, timeout)
                if partial.stat().st_size < entry["bytes"]:
                    raise ConnectionError("the connection ended before the file did")
                break
            except (urllib.error.URLError, http.client.HTTPException, TimeoutError, OSError) as error:
                if isinstance(error, urllib.error.HTTPError) and error.code < 500:
                    raise ValueError(f"{entry['path']} was refused by the server: HTTP {error.code}.") from None
                require(attempt < attempts, f"{entry['path']} could not be downloaded: {error}")
                report(json.dumps({"state": "retrying", "path": entry["path"], "attempt": attempt}))
                time.sleep(2 ** attempt)
        size = partial.stat().st_size
        digest = file_digest(partial).hexdigest()
        if size != entry["bytes"] or digest != entry["sha256"]:
            partial.unlink()
            raise ValueError(f"{entry['path']} did not match its pinned size and SHA-256; the download was discarded.")
        os.replace(partial, final)
        receipts.append({"path": entry["path"], "downloadedBytes": downloaded, "seconds": round(time.monotonic() - began, 3)})
        report(json.dumps({"state": "verified", "path": entry["path"], "bytes": entry["bytes"]}))
    receipt_directory = target / "_revia_download"
    receipt_directory.mkdir(exist_ok=True)
    receipt = {"asset": snapshot["id"], "repository": snapshot["repository"], "revision": snapshot["revision"],
               "approval": snapshot.get("approval", ""), "endpoint": endpoint, "startedUtc": started.isoformat(),
               "finishedUtc": datetime.now(timezone.utc).isoformat(), "python": platform.python_version(), "files": receipts,
               "installManifestSha256": hashlib.sha256(manifest_bytes).hexdigest()}
    stamp = started.strftime("%Y%m%dT%H%M%SZ")
    with (receipt_directory / f"{stamp}-receipt.json").open("x", encoding="utf-8") as output:
        json.dump(receipt, output, indent=2)
    # Written last: its presence means every pinned file was verified.
    with manifest_path.open("xb") as output:
        output.write(manifest_bytes)
    report(json.dumps({"state": "installed", "asset": snapshot["id"], "directory": str(target)}))
    return {"state": "installed", "directory": str(target), "installManifestSha256": receipt["installManifestSha256"]}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--asset", required=True)
    parser.add_argument("--destination", required=True)
    parser.add_argument("--assets", default=str(ASSETS))
    parser.add_argument("--endpoint", default="https://huggingface.co")
    parser.add_argument("--plan", action="store_true", help="Print the pinned files, size and manifest digest without downloading.")
    arguments = parser.parse_args(argv)
    try:
        snapshots = load_assets(arguments.assets)
        require(arguments.asset in snapshots, f"Unknown asset; choose one of: {', '.join(sorted(snapshots))}.")
        snapshot = snapshots[arguments.asset]
        if arguments.plan:
            print(json.dumps({"asset": snapshot["id"], "repository": snapshot["repository"], "revision": snapshot["revision"],
                              "approval": snapshot.get("approval", ""), "files": len(snapshot["files"]),
                              "bytes": sum(entry["bytes"] for entry in snapshot["files"]),
                              "directory": str(target_directory(snapshot, arguments.destination)),
                              "installManifestSha256": hashlib.sha256(install_manifest(snapshot)).hexdigest()}, indent=2))
            return 0
        install(snapshot, arguments.destination, arguments.endpoint)
        return 0
    except ValueError as error:
        print(json.dumps({"state": "failed", "error": str(error)}), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
