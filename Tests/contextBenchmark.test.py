import importlib.util
import json
from pathlib import Path
import unittest
from copy import deepcopy
import re
import io
import tempfile
import threading
from unittest.mock import patch, Mock


MODULE = Path(__file__).resolve().parents[1] / "Tools/Quality/contextBenchmark.py"
spec = importlib.util.spec_from_file_location("context_benchmark", MODULE)
benchmark = importlib.util.module_from_spec(spec)
spec.loader.exec_module(benchmark)


class ContextBenchmarkTests(unittest.TestCase):
    def test_auth_headers_and_failure_bodies_never_echo_secret(self):
        secret = "fixture-secret-token"
        response = Mock(status=401)
        response.read.return_value = ("Echoed " + secret).encode()
        connection = Mock()
        connection.getresponse.return_value = response
        with patch.object(benchmark.http.client, "HTTPConnection", return_value=connection):
            with self.assertRaises(RuntimeError) as caught:
                benchmark.request_json(18080, "/v1/models", api_key=secret)
        self.assertNotIn(secret, str(caught.exception))
        self.assertEqual(connection.request.call_args.kwargs["headers"]["Authorization"], "Bearer " + secret)
        connection.close.assert_called_once()

    def test_ephemeral_key_cleanup_only_removes_owned_file(self):
        with tempfile.TemporaryDirectory() as temporary:
            unrelated = Path(temporary) / "unrelated.txt"
            unrelated.write_text("retain")
            with patch.object(benchmark.tempfile, "tempdir", temporary):
                with self.assertRaisesRegex(RuntimeError, "fixture failure"):
                    with benchmark.EphemeralApiKey() as credential:
                        path = credential.path
                        self.assertEqual(path.read_text().strip(), credential.value)
                        self.assertGreaterEqual(len(credential.value), 64)
                        raise RuntimeError("fixture failure")
                self.assertFalse(path.exists())
            self.assertEqual(unrelated.read_text(), "retain")

    def test_stream_redaction_handles_key_split_across_chunks(self):
        key = "fixture-secret-token"
        output = io.BytesIO()
        class Chunks:
            def __init__(self): self.parts = iter([b"before fixture-sec", b"ret-token after", b""])
            def read(self, _): return next(self.parts)
        benchmark.stream_redacted(Chunks(), output, key)
        self.assertNotIn(key.encode(), output.getvalue())
        self.assertEqual(output.getvalue(), b"before [REDACTED] after")

    def test_readiness_proves_authentication_exact_identity_and_capacity(self):
        calls = []
        def request(port, path, payload=None, timeout=180, *, api_key=None):
            calls.append((path, api_key))
            if path == "/health": return {"status": "ok"}
            if path == "/v1/models":
                # Current llama.cpp inventory routes are public even with API keys.
                return {"data": [{"id": "exact-fixture"}]}
            if path == "/tokenize":
                if api_key != "owned-key": raise benchmark.BackendHttpError(path, 401)
                self.assertEqual(payload, {"content": "evaluation-auth-probe"})
                return {"tokens": [42]}
            return {"default_generation_settings": {"n_ctx": 16384}, "total_slots": 1}
        process = Mock(); process.poll.return_value = None
        with patch.object(benchmark, "request_json", side_effect=request):
            benchmark.wait_ready(process, 18080, "exact-fixture", 16384, api_key="owned-key")
        self.assertEqual([path for path, _ in calls], ["/health", "/tokenize", "/tokenize", "/v1/models", "/props"])
        self.assertTrue(all(key == "owned-key" for index, (_, key) in enumerate(calls) if index != 1))
        self.assertNotEqual(calls[1][1], "owned-key")
        with patch.object(benchmark, "request_json", return_value={"status": "ok"}):
            with self.assertRaisesRegex(RuntimeError, "incorrect API key"):
                benchmark.wait_ready(process, 18080, "exact-fixture", 16384, api_key="owned-key")

    def test_success_response_with_credential_is_withheld_even_if_escaped(self):
        key = "fixture-secret-token"
        response = Mock(status=200)
        response.read.return_value = ('{"unexpected":"' + "".join('\\u%04x' % ord(ch) for ch in key) + '"}').encode()
        connection = Mock(); connection.getresponse.return_value = response
        with patch.object(benchmark.http.client, "HTTPConnection", return_value=connection):
            with self.assertRaisesRegex(RuntimeError, "withheld") as caught:
                benchmark.request_json(18080, "/props", api_key=key)
        self.assertNotIn(key, str(caught.exception))

    def test_owned_run_redacts_failed_readiness_and_logs_then_removes_key(self):
        process = Mock(pid=123)
        process.poll.return_value = 0
        captured = {}
        def launch(arguments, **kwargs):
            captured["key_path"] = Path(arguments[arguments.index("--api-key-file") + 1])
            captured["key"] = captured["key_path"].read_text().strip()
            process.stdout = io.BytesIO(("startup echo: " + captured["key"] + "\n").encode())
            self.assertEqual(kwargs["stdout"], benchmark.subprocess.PIPE)
            return process
        def ready(*args, api_key=None):
            self.assertEqual(api_key, captured["key"])
            raise RuntimeError("unexpected server echo " + api_key)
        class Sampler:
            def __init__(self): self.samples=[]; self.stopped=threading.Event(); self.worker=Mock()
            def __enter__(self): return self
            def __exit__(self, *_): pass
        with tempfile.TemporaryDirectory() as temporary:
            evidence = Path(temporary) / "evidence"
            with patch.object(benchmark, "require_api_key_file_support"), patch.object(benchmark, "gpu_snapshot", return_value={}), \
                 patch.object(benchmark, "GpuSampler", Sampler), patch.object(benchmark.subprocess, "Popen", side_effect=launch), \
                 patch.object(benchmark, "wait_ready", side_effect=ready), patch.object(benchmark, "stop_owned_process") as stop:
                with self.assertRaises(RuntimeError) as caught:
                    benchmark.run_context(Path("server.exe"), Path("model.gguf"), evidence, 16384, "CUDA0")
            self.assertNotIn(captured["key"], str(caught.exception))
            self.assertFalse(captured["key_path"].exists())
            self.assertFalse(captured["key_path"].is_relative_to(evidence))
            stop.assert_called_once_with(process)
            for path in evidence.iterdir():
                self.assertNotIn(captured["key"].encode(), path.read_bytes(), path.name)
            self.assertIn("[REDACTED]", (evidence / "server.log").read_text())

    def test_server_key_file_flag_requires_advertised_support_and_preserves_decoding(self):
        old = benchmark.server_arguments(Path("server"), Path("model"), 16384, 18080, "alias", "CUDA0")
        new = benchmark.server_arguments(Path("server"), Path("model"), 16384, 18080, "alias", "CUDA0", api_key_file=Path("owned-key"))
        self.assertEqual(new, old + ["--api-key-file", "owned-key"])
        with patch.object(benchmark.subprocess, "run", return_value=Mock(returncode=0, stdout="--api-key-file FNAME", stderr="")):
            benchmark.require_api_key_file_support(Path("server"))
        with patch.object(benchmark.subprocess, "run", return_value=Mock(returncode=0, stdout="", stderr="")):
            with self.assertRaisesRegex(RuntimeError, "advertise"):
                benchmark.require_api_key_file_support(Path("server"))

    def test_log_finalization_failure_cannot_leave_completed_evidence(self):
        class FailedLog:
            def __init__(self, *_): pass
            def __enter__(self): return self
            def capture(self, _): pass
            def __exit__(self, *_): raise RuntimeError("fixture log finalization failed")
        class Sampler:
            def __init__(self): self.samples=[]; self.stopped=threading.Event(); self.worker=Mock()
            def __enter__(self): return self
            def __exit__(self, *_): pass
        process = Mock(pid=123); process.poll.return_value = 0
        with tempfile.TemporaryDirectory() as temporary:
            evidence = Path(temporary) / "evidence"
            with patch.object(benchmark, "require_api_key_file_support"), patch.object(benchmark, "gpu_snapshot", return_value={}), \
                 patch.object(benchmark, "GpuSampler", Sampler), patch.object(benchmark, "RedactingProcessLog", FailedLog), \
                 patch.object(benchmark.subprocess, "Popen", return_value=process), patch.object(benchmark, "PROBES", ()), \
                 patch.object(benchmark, "wait_ready", return_value={}), patch.object(benchmark, "stop_owned_process"):
                with self.assertRaisesRegex(RuntimeError, "log finalization failed"):
                    benchmark.run_context(Path("server"), Path("model"), evidence, 16384, "CUDA0")
            result = json.loads((evidence / "results.json").read_text())
            self.assertEqual(result["status"], "failed")
            self.assertIn("log finalization failed", result["error"])

    def test_schema_enforces_output_shape_without_supplying_the_correct_answer(self):
        for probe in benchmark.PROBES:
            request = benchmark.completion_request("fixture", probe, benchmark.messages_for(probe, 2))
            response_format = request["response_format"]
            self.assertEqual(response_format["type"], "json_schema")
            self.assertTrue(response_format["json_schema"]["strict"])
            schema = response_format["json_schema"]["schema"]
            self.assertEqual(schema["type"], "object")
            self.assertFalse(schema["additionalProperties"])
            self.assertEqual(set(schema["required"]), set(probe["expected"]))
            source_key = next(key for key in probe["expected"] if "source" in key)
            visible_ids = re.findall(r"Source ([A-Z][0-9]+):", probe["initial"] + probe["correction"])
            self.assertEqual(schema["properties"][source_key], {"type": "string", "enum": visible_ids})
            self.assertGreater(len(visible_ids), 1)
            self.assertNotIn("const", json.dumps(schema))
            for key, value in probe["expected"].items():
                self.assertEqual(schema["properties"][key]["type"], "integer" if type(value) is int else "string")
            changed = deepcopy(probe)
            changed["expected"] = {"arbitrary": "This must never become a decoding constraint."}
            self.assertEqual(benchmark.completion_request("fixture", changed, [])["response_format"], response_format)
            control = benchmark.completion_request("fixture", probe, [], "json_object")
            self.assertEqual(control["response_format"], {"type": "json_object"})
            # Compare only the format switch: decoding, messages and model stay identical.
            schema_request = benchmark.completion_request("fixture", probe, [])
            del schema_request["response_format"]
            del control["response_format"]
            self.assertEqual(schema_request, control)

    def test_source_spelling_and_duplicate_keys_remain_strict_failures(self):
        for probe in benchmark.PROBES:
            expected = probe["expected"]
            source_key = next(key for key in expected if "source" in key)
            source = expected[source_key]
            for invalid in (2, "Source " + source, source + " ", source[0] + "1"):
                self.assertFalse(benchmark.grade(json.dumps(dict(expected, **{source_key: invalid})), "stop", expected)["passed"])
            duplicate = json.dumps(expected)[:-1] + "," + json.dumps(source_key) + ":" + json.dumps(source) + "}"
            self.assertFalse(benchmark.grade(duplicate, "stop", expected)["passed"])

    def test_cache_context_and_format_controls_preserve_defaults(self):
        required = ["--server", "server.exe", "--model", "model.gguf", "--output", "new-run"]
        defaults = benchmark.argument_parser().parse_args(required)
        self.assertEqual(defaults.contexts, [16384, 32768])
        self.assertEqual((defaults.cache_type_k, defaults.cache_type_v), ("f16", "f16"))
        selected = benchmark.argument_parser().parse_args(required + ["--contexts", "32768", "--cache-type-k", "q8_0",
            "--cache-type-v", "q8_0", "--response-format", "json_object"])
        self.assertEqual(selected.contexts, [32768])
        self.assertEqual(selected.response_format, "json_object")
        arguments = benchmark.server_arguments(Path("server.exe"), Path("model.gguf"), 32768, 18081, "fixture", "CUDA0",
                                               selected.cache_type_k, selected.cache_type_v)
        self.assertEqual(arguments[arguments.index("--cache-type-k") + 1], "q8_0")
        self.assertEqual(arguments[arguments.index("--cache-type-v") + 1], "q8_0")

    def test_late_corrections_and_distractors_remain_in_both_contexts(self):
        for probe in benchmark.PROBES:
            small = benchmark.messages_for(probe, 20)
            large = benchmark.messages_for(probe, 40)
            self.assertEqual(small[-1], large[-1])
            self.assertEqual(small[1], large[1])
            content = [message["content"] for message in large]
            correction = content.index(probe["correction"])
            self.assertGreater(correction, len(content) * 0.65)
            self.assertLess(correction, len(content) - 3)
            self.assertEqual(len([text for text in content if text.startswith("Archive note ")]), 40)
            self.assertNotIn("expected", json.dumps(large))

    def test_exact_grade_rejects_old_facts_extra_fields_and_truncation(self):
        expected = benchmark.PROBES[0]["expected"]
        self.assertTrue(benchmark.grade(json.dumps(expected), "stop", expected)["passed"])
        stale = dict(expected, city="Lisbon")
        self.assertFalse(benchmark.grade(json.dumps(stale), "stop", expected)["passed"])
        extra = dict(expected, unknown="guess")
        self.assertFalse(benchmark.grade(json.dumps(extra), "stop", expected)["passed"])
        self.assertFalse(benchmark.grade(json.dumps(expected), "length", expected)["passed"])
        self.assertFalse(benchmark.grade("No idea", "stop", expected)["passed"])

    def test_fit_uses_backend_rendering_special_tokens_and_reserved_output(self):
        calls = []

        def post(path, payload):
            calls.append((path, payload))
            if path == "/apply-template":
                self.assertFalse(payload["chat_template_kwargs"]["enable_thinking"])
                self.assertTrue(payload["add_generation_prompt"])
                return {"prompt": "x" * (200 + len(payload["messages"]) * 37)}
            self.assertEqual(path, "/tokenize")
            self.assertTrue(payload["add_special"])
            self.assertTrue(payload["parse_special"])
            return {"tokens": [1] * len(payload["content"])}

        result = benchmark.fit_probe(post, "fixture", benchmark.PROBES[0], 4096, 192)
        self.assertLessEqual(result["promptTokens"], result["targetTokens"])
        self.assertGreater(result["promptTokens"], result["targetTokens"] - 100)
        self.assertLess(result["promptTokens"] + 192, 4096)
        self.assertEqual(len(calls) % 2, 0)

    def test_fit_fails_closed_when_template_or_tokens_missing(self):
        with self.assertRaisesRegex(RuntimeError, "prompt"):
            benchmark.fit_probe(lambda *_: {}, "fixture", benchmark.PROBES[0], 4096, 192)
        with self.assertRaisesRegex(RuntimeError, "tokens"):
            benchmark.fit_probe(lambda path, _: {"prompt": "hello"} if path == "/apply-template" else {},
                                "fixture", benchmark.PROBES[0], 4096, 192)

    def test_gpu_csv_keeps_devices_separate(self):
        values = benchmark.parse_gpu_csv("0, RTX 5070, 12227, 3500\n1, RTX 2070 SUPER, 8192, 0\n")
        self.assertEqual(values[0]["usedMiB"], 3500)
        self.assertEqual(values[1]["usedMiB"], 0)
        self.assertEqual(values[0]["totalMiB"], 12227)
        with self.assertRaises(ValueError):
            benchmark.parse_gpu_csv("0, RTX 5070, N/A, N/A")


if __name__ == "__main__":
    unittest.main()
