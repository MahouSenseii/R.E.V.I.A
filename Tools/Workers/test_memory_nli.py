"""NLI transport/input contracts; model quality needs separate labelled evidence."""
import importlib.util
import hashlib
import json
from pathlib import Path
import socket
import tempfile
import threading
import time
import unittest
from unittest.mock import Mock, patch
from urllib import request as http
from urllib.error import HTTPError

spec = importlib.util.spec_from_file_location("memory_nli", Path(__file__).with_name("memory_nli.py"))
worker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(worker)


def model_fixture():
    model = Mock()
    model.tokenizer.return_value = {"input_ids": [[1, 2], [2, 1]]}
    model.predict.return_value = [[0.001, 0.995, 0.004], [0.002, 0.990, 0.008]]
    model.model.config.id2label = {0: "contradiction", 1: "entailment", 2: "neutral"}
    return model


class NliTests(unittest.TestCase):
    def test_both_directions_keep_explicit_label_meanings(self):
        model = model_fixture()
        result = worker.score_request(model, {"left": "A", "right": "B"})
        self.assertEqual(result["model"], worker.MODEL_IDENTITY)
        self.assertFalse(result["truncated"])
        self.assertEqual(result["forward"], {"entailment": 0.995, "contradiction": 0.001})
        self.assertEqual(result["reverse"], {"entailment": 0.990, "contradiction": 0.002})
        self.assertEqual(model.predict.call_args.args[0], [("A", "B"), ("B", "A")])
        self.assertTrue(model.predict.call_args.kwargs["apply_softmax"])
        self.assertFalse(model.tokenizer.call_args.kwargs["truncation"])

    def test_bad_input_and_overflow_never_reach_inference(self):
        for value in [{}, {"left": "", "right": "B"}, {"left": "A", "right": 2},
                      {"left": "A", "right": "B", "url": "extra"},
                      {"left": "a" * 1025, "right": "B"}, {"left": "é" * 513, "right": "B"},
                      {"left": "bad\0text", "right": "B"}]:
            model = model_fixture()
            with self.subTest(value=str(value)[:60]), self.assertRaises(ValueError):
                worker.score_request(model, value)
            model.predict.assert_not_called()
        model = model_fixture()
        model.tokenizer.return_value = {"input_ids": [[0] * 513, [0] * 512]}
        with self.assertRaises(ValueError):
            worker.score_request(model, {"left": "A", "right": "B"})
        model.predict.assert_not_called()

    def test_incomplete_nonfinite_or_nonprobability_results_are_rejected(self):
        for scores in [[], [[0, 1, 0]], [[0, 1, 0], [0, 1]],
                       [[0, 1, 0], [float("nan"), 1, 0]],
                       [[0, 1, 0], [-0.1, 1.1, 0]],
                       [[0, 1, 0], [0.1, 0.1, 0.1]]]:
            model = model_fixture()
            model.predict.return_value = scores
            with self.subTest(scores=scores), self.assertRaises(ValueError):
                worker.score_request(model, {"left": "A", "right": "B"})

    def test_offline_cpu_safetensors_and_label_binding(self):
        factory = Mock(return_value=model_fixture())
        torch = Mock()
        with tempfile.TemporaryDirectory() as directory:
            with patch.object(worker, "verify_model_files"), patch.dict(
                    "sys.modules", {"torch": torch, "sentence_transformers": Mock(CrossEncoder=factory)}):
                worker.load_model(Path(directory), 2)
                self.assertEqual(factory.call_args.kwargs["device"], "cpu")
                self.assertTrue(factory.call_args.kwargs["local_files_only"])
                self.assertFalse(factory.call_args.kwargs["trust_remote_code"])
                self.assertTrue(factory.call_args.kwargs["model_kwargs"]["use_safetensors"])
                factory.return_value.model.config.id2label = {0: "entailment", 1: "contradiction", 2: "neutral"}
                with self.assertRaises(ValueError):
                    worker.load_model(Path(directory), 2)
        torch.set_num_interop_threads.assert_called()

    def test_model_provenance_rejects_missing_or_foreign_installation(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            with self.assertRaises(FileNotFoundError):
                worker.verify_model_files(path)
            (path / "installation.json").write_text(json.dumps({"model": "other", "revision": "other"}))
            with self.assertRaises(ValueError):
                worker.verify_model_files(path)
            (path / "installation.json").write_text(json.dumps({
                "model": "cross-encoder/nli-deberta-v3-small", "revision": worker.MODEL_REVISION,
                "files": [{"file": "../outside", "sha256": "", "bytes": 0}]}))
            with self.assertRaises(ValueError):
                worker.verify_model_files(path)

    def test_model_provenance_rejects_unlisted_tokenizer_artifacts(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            files = []
            for name in ("model.safetensors", "config.json", "tokenizer.json", "tokenizer_config.json"):
                data = ("synthetic-" + name).encode()
                (path / name).write_bytes(data)
                files.append({"file": name, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()})
            manifest = {"model": "cross-encoder/nli-deberta-v3-small", "revision": worker.MODEL_REVISION, "files": files}
            (path / "installation.json").write_text(json.dumps(manifest))
            with patch.object(worker, "WEIGHTS_SHA256", files[0]["sha256"]):
                worker.verify_model_files(path)
                for name in ("added_tokens.json", "special_tokens_map.json", "spm.model"):
                    with self.subTest(name=name):
                        extra = path / name
                        extra.write_bytes(b"unrecorded tokenizer content")
                        with self.assertRaises(ValueError):
                            worker.verify_model_files(path)
                        extra.unlink()

    def test_auth_rejection_consumes_only_bounded_body_before_closing(self):
        model = model_fixture()
        with worker.LocalServer(("127.0.0.1", 0), worker.make_handler(model, "private-key")) as server:
            thread = threading.Thread(target=server.serve_forever)
            thread.start()
            try:
                body = b"x" * worker.MAX_REQUEST_BYTES
                with socket.create_connection(("127.0.0.1", server.server_port), timeout=2) as connection:
                    connection.sendall(("POST /compare HTTP/1.1\r\nHost: localhost\r\nContent-Length: " +
                                        str(len(body)) + "\r\n\r\n").encode())
                    # Header/body arrival can be separate TCP packets. Replying and
                    # closing before this bounded body arrives caused Windows 10053.
                    time.sleep(0.05)
                    connection.sendall(body)
                    response = b""
                    while data := connection.recv(4096):
                        response += data
                self.assertTrue(response.startswith(b"HTTP/1.0 401"), response[:100])
                self.assertEqual(json.loads(response.split(b"\r\n\r\n", 1)[1]), {"error": "Unauthorized."})
                model.predict.assert_not_called()
                model.tokenizer.assert_not_called()
            finally:
                server.shutdown()
                thread.join()

    def test_http_requires_auth_and_does_not_echo_private_input(self):
        model = model_fixture()
        with worker.LocalServer(("127.0.0.1", 0), worker.make_handler(model, "private-key")) as server:
            thread = threading.Thread(target=server.serve_forever)
            thread.start()
            try:
                url = f"http://127.0.0.1:{server.server_port}/compare"
                payload = json.dumps({"left": "private left", "right": "private right"}).encode()
                with self.assertRaises(HTTPError) as error:
                    http.urlopen(http.Request(url, data=payload), timeout=2)
                self.assertEqual(error.exception.code, 401)
                model.predict.assert_not_called()
                headers = {"Authorization": "Bearer private-key"}
                with http.urlopen(http.Request(url, data=payload, headers=headers), timeout=2) as response:
                    self.assertEqual(json.load(response)["model"], worker.MODEL_IDENTITY)
                model.predict.side_effect = RuntimeError("private left")
                with self.assertRaises(HTTPError) as error:
                    http.urlopen(http.Request(url, data=payload, headers=headers), timeout=2)
                self.assertEqual(error.exception.code, 503)
                self.assertNotIn(b"private left", error.exception.read())
            finally:
                server.shutdown()
                thread.join()

    def test_unauthenticated_oversize_body_is_not_drained(self):
        model = model_fixture()
        with worker.LocalServer(("127.0.0.1", 0), worker.make_handler(model, "private-key")) as server:
            thread = threading.Thread(target=server.serve_forever)
            thread.start()
            try:
                with socket.create_connection(("127.0.0.1", server.server_port), timeout=0.5) as connection:
                    connection.sendall(("POST /compare HTTP/1.1\r\nHost: localhost\r\nContent-Length: " +
                                        str(worker.MAX_REQUEST_BYTES + 1) + "\r\n\r\n").encode())
                    self.assertTrue(connection.recv(4096).startswith(b"HTTP/1.0 401"))
                model.predict.assert_not_called()
            finally:
                server.shutdown()
                thread.join()


if __name__ == "__main__":
    unittest.main()
