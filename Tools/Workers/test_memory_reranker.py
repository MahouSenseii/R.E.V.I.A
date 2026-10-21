"""Protocol and CPU/offline loading checks; these do not measure model quality."""
import importlib.util
import json
import tempfile
import threading
import unittest
from urllib import request as http
from urllib.error import HTTPError
from pathlib import Path
from unittest.mock import Mock, patch

MODULE = Path(__file__).with_name("memory_reranker.py")
spec = importlib.util.spec_from_file_location("memory_reranker", MODULE)
worker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(worker)


class RerankerTests(unittest.TestCase):
    def test_scores_keep_original_indices_and_raw_logits(self):
        model = Mock()
        model.predict.return_value = [-4.25, 8.5]
        result = worker.score_request(model, {"query": "where", "documents": ["wrong", "right"]})
        self.assertEqual(result, {"scores": [{"index": 0, "score": -4.25}, {"index": 1, "score": 8.5}]})
        self.assertEqual(model.predict.call_args.args[0], [("where", "wrong"), ("where", "right")])

    def test_invalid_requests_never_reach_model(self):
        model = Mock()
        invalid = [
            {}, {"query": "", "documents": ["a"]}, {"query": "q", "documents": []},
            {"query": "q", "documents": ["a"] * 31}, {"query": "q", "documents": [7]},
            {"query": "q", "documents": ["a"], "url": "https://example.com"},
            {"query": "q" * 4097, "documents": ["a"]},
            {"query": "q", "documents": ["a" * 8193]},
            {"query": "q", "documents": ["bad\0value"]},
        ]
        for request in invalid:
            with self.subTest(request=str(request)[:100]), self.assertRaises(ValueError):
                worker.score_request(model, request)
        model.predict.assert_not_called()

    def test_model_must_return_exactly_one_finite_scalar_per_document(self):
        model = Mock()
        for scores in [[1.0], [1.0, float("nan")], [float("inf"), 2.0], [[1.0], [2.0]]]:
            model.predict.return_value = scores
            with self.subTest(scores=scores), self.assertRaises(ValueError):
                worker.score_request(model, {"query": "q", "documents": ["a", "b"]})

    def test_loading_requires_existing_local_model_and_cpu(self):
        torch = Mock()
        factory = Mock()
        with tempfile.TemporaryDirectory() as directory:
            with patch.dict("sys.modules", {"torch": torch, "sentence_transformers": Mock(CrossEncoder=factory)}):
                worker.load_model(Path(directory), 3)
        self.assertEqual(factory.call_args.kwargs["device"], "cpu")
        self.assertTrue(factory.call_args.kwargs["local_files_only"])
        self.assertFalse(factory.call_args.kwargs["trust_remote_code"])
        self.assertTrue(factory.call_args.kwargs["model_kwargs"]["use_safetensors"])
        torch.set_num_threads.assert_called_once_with(3)
        with self.assertRaises(ValueError):
            worker.load_model(Path("cross-encoder/ms-marco-MiniLM-L6-v2"), 4)

    def test_authenticated_http_admits_only_bounded_scoring(self):
        model = Mock()
        model.predict.return_value = [2.5]
        with worker.LocalServer(("127.0.0.1", 0), worker.make_handler(model, "secret")) as server:
            thread = threading.Thread(target=server.serve_forever)
            thread.start()
            try:
                url = f"http://127.0.0.1:{server.server_port}/rerank"
                payload = json.dumps({"query": "q", "documents": ["d"]}).encode()
                with self.assertRaises(HTTPError) as error:
                    http.urlopen(http.Request(url, data=payload), timeout=2)
                self.assertEqual(error.exception.code, 401)
                model.predict.assert_not_called()
                accepted = http.Request(url, data=payload, headers={"Authorization": "Bearer secret"})
                with http.urlopen(accepted, timeout=2) as response:
                    self.assertEqual(json.load(response)["scores"], [{"index": 0, "score": 2.5}])
                with self.assertRaises(HTTPError) as error:
                    invalid = http.Request(url, data=b"x",
                                           headers={"Authorization": "Bearer secret",
                                                    "Content-Length": str(worker.MAX_REQUEST_BYTES + 1)})
                    http.urlopen(invalid, timeout=2)
                self.assertEqual(error.exception.code, 400)
                self.assertEqual(model.predict.call_count, 1)
            finally:
                server.shutdown()
                thread.join()


if __name__ == "__main__":
    unittest.main()
