import hashlib
import http.server
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import threading
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


INSTALLER = load("install_training_assets", ROOT / "Tools" / "install_training_assets.py")
WORKER = load("revia_training_worker", ROOT / "Tools" / "revia_training_worker.py")


def sha(data):
    return hashlib.sha256(data).hexdigest()


class Hub:
    """A local stand-in for a model hub: resolve URLs redirect to a CDN path that honors Range."""

    def __init__(self, files, drop_once=None, corrupt=None):
        self.files, self.drop_once, self.corrupt, self.requests = files, drop_once, corrupt, []
        hub = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *arguments):
                pass

            def do_GET(self):
                hub.requests.append((self.path, self.headers.get("Range")))
                parts = self.path.strip("/").split("/")
                if len(parts) == 5 and parts[2] == "resolve":
                    self.send_response(302)
                    self.send_header("Location", f"/cdn/{parts[4]}")
                    self.end_headers()
                    return
                name = parts[-1]
                data = hub.files[name]
                if name == hub.corrupt:
                    data = bytes([data[0] ^ 1]) + data[1:]
                start = int(self.headers["Range"].split("=")[1].rstrip("-")) if self.headers.get("Range") else 0
                self.send_response(206 if start else 200)
                self.send_header("Content-Length", str(len(data) - start))
                self.end_headers()
                if name == hub.drop_once:
                    hub.drop_once = None
                    self.wfile.write(data[start:start + len(data) // 3])
                    self.close_connection = True
                    return
                self.wfile.write(data[start:])

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.endpoint = f"http://127.0.0.1:{self.server.server_address[1]}"

    def close(self):
        self.server.shutdown()
        self.server.server_close()

    def fetched(self, name):
        return [entry for entry in self.requests if entry[0].endswith("/" + name)]


class TrainingAssetInstallerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.destination = Path(self.temporary.name) / "models"
        config = {"architectures": ["Qwen3_5ForConditionalGeneration"],
                  "text_config": {"model_type": "qwen3_5_text", "hidden_size": 8, "num_hidden_layers": 2, "vocab_size": 32}}
        self.files = {"config.json": json.dumps(config).encode(), "chat_template.jinja": b"{{ messages }}",
                      "model.safetensors": os.urandom(300000) + b"end"}
        self.snapshot = {"id": "fixture-base", "repository": "org/model", "revision": "a" * 40, "approval": "fixture",
                         "files": [{"path": name, "bytes": len(data), "sha256": sha(data)} for name, data in sorted(self.files.items())]}
        self.target = INSTALLER.target_directory(self.snapshot, self.destination)
        self.sleep = mock.patch.object(INSTALLER.time, "sleep")
        self.sleep.start()
        self.small_chunks = mock.patch.object(INSTALLER, "CHUNK", 65536)
        self.small_chunks.start()
        self.margin = mock.patch.object(INSTALLER, "MARGIN", 0)
        self.margin.start()

    def tearDown(self):
        mock.patch.stopall()
        self.temporary.cleanup()

    def install(self, hub):
        return INSTALLER.install(self.snapshot, self.destination, hub.endpoint, timeout=10, report=lambda line: None)

    def test_installs_verified_files_and_a_manifest_the_worker_accepts(self):
        hub = Hub(self.files)
        try:
            result = self.install(hub)
            for name, data in self.files.items():
                self.assertEqual((self.target / name).read_bytes(), data)
            manifest = (self.target / INSTALLER.INSTALL_MANIFEST).read_bytes()
            self.assertEqual(manifest, INSTALLER.install_manifest(self.snapshot))
            self.assertEqual(result["installManifestSha256"], sha(manifest))
            self.assertEqual(len(list((self.target / "_revia_download").glob("*-receipt.json"))), 1)
            identity = WORKER.verify_base({"base": {"directory": str(self.target), "repository": "org/model",
                                                    "revision": "a" * 40, "installManifestSha256": sha(manifest)}})
            self.assertEqual((identity["modelType"], identity["templateSha256"]), ("qwen3_5_text", sha(b"{{ messages }}")))
            before = len(hub.requests)
            self.assertEqual(self.install(hub)["state"], "installed")
            self.assertEqual(len(hub.requests), before, "An installed snapshot was downloaded again.")
        finally:
            hub.close()

    def test_dropped_connection_resumes_with_a_range(self):
        hub = Hub(self.files, drop_once="model.safetensors")
        try:
            self.install(hub)
            requests = hub.fetched("model.safetensors")
            self.assertTrue(any(header and header.startswith("bytes=") and header != "bytes=0-" for _, header in requests), requests)
            self.assertEqual((self.target / "model.safetensors").read_bytes(), self.files["model.safetensors"])
        finally:
            hub.close()

    def test_corrupted_download_is_discarded(self):
        hub = Hub(self.files, corrupt="model.safetensors")
        try:
            with self.assertRaisesRegex(ValueError, "did not match its pinned size and SHA-256"):
                self.install(hub)
            self.assertFalse((self.target / "model.safetensors").exists())
            self.assertFalse((self.target / "model.safetensors.partial").exists())
            self.assertFalse((self.target / INSTALLER.INSTALL_MANIFEST).exists())
        finally:
            hub.close()

    def test_existing_different_file_is_never_replaced(self):
        self.target.mkdir(parents=True)
        (self.target / "config.json").write_bytes(b"{}")
        hub = Hub(self.files)
        try:
            with self.assertRaisesRegex(ValueError, "not replaced"):
                self.install(hub)
            self.assertEqual((self.target / "config.json").read_bytes(), b"{}")
        finally:
            hub.close()

    def test_complete_partial_is_verified_without_downloading_again(self):
        self.target.mkdir(parents=True)
        (self.target / "model.safetensors.partial").write_bytes(self.files["model.safetensors"])
        hub = Hub(self.files)
        try:
            self.install(hub)
            self.assertEqual(hub.fetched("model.safetensors"), [])
            self.assertEqual((self.target / "model.safetensors").read_bytes(), self.files["model.safetensors"])
        finally:
            hub.close()

    def test_onedrive_destination_is_refused(self):
        with mock.patch.dict(os.environ, {"OneDrive": self.temporary.name}):
            with self.assertRaisesRegex(ValueError, "outside OneDrive"):
                INSTALLER.install(self.snapshot, self.destination, "http://127.0.0.1:9", report=lambda line: None)

    def test_asset_list_is_validated_and_ships_the_approved_snapshots(self):
        snapshots = INSTALLER.load_assets()
        self.assertEqual({key: value["revision"] for key, value in snapshots.items()}, {
            "qwen3.5-9b-base": "c202236235762e1c871ad0ccb60c8ee5ba337b9a",
            "qwen3.5-9b-dflash": "5fc3b3d474760f18c516db87d84c37edbfd3ede6",
            "qwen3.5-4b-base": "851bf6e806efd8d0a36b00ddf55e13ccb7b8cd0a"})
        for base in ("qwen3.5-9b-base", "qwen3.5-4b-base"):
            names = {entry["path"] for entry in snapshots[base]["files"]}
            self.assertTrue({"config.json", "chat_template.jinja", "tokenizer.json"} <= names)
        broken = Path(self.temporary.name) / "assets.json"
        for change in ({"path": "nested/file.bin"}, {"sha256": "0" * 63}, {"bytes": 0}):
            entry = {**self.snapshot["files"][0], **change}
            broken.write_text(json.dumps({"schemaVersion": 1, "snapshots": [{**self.snapshot, "files": [entry]}]}))
            with self.assertRaises(ValueError):
                INSTALLER.load_assets(broken)


if __name__ == "__main__":
    unittest.main(verbosity=2)
