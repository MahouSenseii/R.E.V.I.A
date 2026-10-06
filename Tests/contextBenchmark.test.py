import importlib.util
import json
from pathlib import Path
import unittest


MODULE = Path(__file__).resolve().parents[1] / "Tools/Quality/contextBenchmark.py"
spec = importlib.util.spec_from_file_location("context_benchmark", MODULE)
benchmark = importlib.util.module_from_spec(spec)
spec.loader.exec_module(benchmark)


class ContextBenchmarkTests(unittest.TestCase):
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
