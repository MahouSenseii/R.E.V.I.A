import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("nli_benchmark", ROOT / "Tools/Quality/nliDedupBenchmark.py")
benchmark = importlib.util.module_from_spec(spec)
spec.loader.exec_module(benchmark)


def case(name="one", split="development", label="equivalent"):
    return {"id": name, "split": split, "label": label, "category": "fixture", "left": "A", "right": "B",
            "note": "Synthetic fixture", "precheckMustReject": True}


def fixture_model(rows=None):
    model = Mock()
    model.tokenizer.return_value = {"input_ids": [[1, 2], [2, 1]]}
    model.predict.return_value = rows or [[0.001, 0.995, 0.004], [0.002, 0.990, 0.008]]
    return model


class NliBenchmarkTests(unittest.TestCase):
    def test_worker_path_keeps_raw_labels_both_directions_and_guard_separate(self):
        model = fixture_model()
        with patch.object(benchmark.worker, "score_request", wraps=benchmark.worker.score_request) as actual_worker:
            record = benchmark.run_cases([case()], model)[0]
        actual_worker.assert_called_once()
        self.assertEqual(model.predict.call_args.args[0], [("A", "B"), ("B", "A")])
        self.assertEqual(record["rawProbabilities"]["forward"],
                         {"contradiction": 0.001, "entailment": 0.995, "neutral": 0.004})
        self.assertEqual(record["rawProbabilities"]["reverse"]["entailment"], 0.990)
        self.assertTrue(record["modelThresholdPass"])
        self.assertTrue(record["declaredPrecheckMustReject"])
        self.assertFalse(record["deterministicGuardInvoked"])
        self.assertGreaterEqual(record["latencyMilliseconds"], 0)

    def test_fixed_rule_requires_both_directions_and_contradiction_limit(self):
        for rows, expected in [([[0.01, 0.98, 0.01], [0.01, 0.98, 0.01]], True),
                               ([[0.001, 0.995, 0.004], [0.002, 0.979, 0.019]], False),
                               ([[0.011, 0.985, 0.004], [0.002, 0.990, 0.008]], False)]:
            with self.subTest(rows=rows):
                result = benchmark.run_cases([case()], fixture_model(rows))[0]
                self.assertEqual(result["modelThresholdPass"], expected)

    def test_split_confusion_excludes_uncertain_and_errors_from_labelled_denominator(self):
        records = []
        for name, split, label, passed in [("tp", "development", "equivalent", True),
                ("fp", "development", "distinct", True), ("u", "development", "uncertain", True),
                ("fn", "heldout", "equivalent", False), ("tn", "heldout", "distinct", False)]:
            records.append(dict(case(name, split, label), status="scored", modelThresholdPass=passed,
                                latencyMilliseconds=2.0, deterministicGuardInvoked=False))
        records.append(dict(case("error", "heldout", "distinct"), status="error", latencyMilliseconds=1.0))
        summary = benchmark.summarize(records)
        self.assertEqual(summary["development"]["labelledConfusion"], {"tp": 1, "fp": 1, "fn": 0, "tn": 0})
        self.assertEqual(summary["development"]["modelOnlyPrecision"], 0.5)
        self.assertEqual(summary["development"]["uncertain"]["thresholdPassIds"], ["u"])
        self.assertEqual(summary["heldout"]["labelledConfusion"], {"tp": 0, "fp": 0, "fn": 1, "tn": 1})
        self.assertEqual(summary["heldout"]["errorIds"], ["error"])
        self.assertIsNone(summary["heldout"]["modelOnlyPrecision"])

    def test_invalid_worker_results_are_errors_not_negative_predictions(self):
        record = benchmark.run_cases([case()], fixture_model([[0, 1, 0], [float("nan"), 1, 0]]))[0]
        self.assertEqual(record["status"], "error")
        self.assertNotIn("modelThresholdPass", record)
        self.assertEqual(benchmark.summarize([record])["development"]["labelledConfusion"],
                         {"tp": 0, "fp": 0, "fn": 0, "tn": 0})

    def test_corpus_rejects_unknown_labels_and_cross_split_pair_leakage(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "pairs.json"
            for cases in [[case(label="paraphrase")], [case(), case("other", "heldout")],
                          [case(), case()], [dict(case(), precheckMustReject="false")]]:
                path.write_text(json.dumps({"version": 1, "cases": cases}))
                with self.subTest(cases=cases), self.assertRaises(ValueError):
                    benchmark.load_corpus(path)
        frozen = benchmark.load_corpus(ROOT / "Tests/Data/semanticDuplicatePairs.json")
        self.assertEqual(len(frozen["cases"]), 72)
        self.assertEqual(sum(row["split"] == "heldout" for row in frozen["cases"]), 32)
        self.assertEqual(sum("legacy" in row for row in frozen["cases"]), 24)

    def test_preflight_records_hashes_without_loading_model_and_refuses_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            model = base / "model"
            model.mkdir()
            manifest = {"model": "fixture", "revision": benchmark.worker.MODEL_REVISION, "files": []}
            (model / "installation.json").write_text(json.dumps(manifest))
            output = base / "run"
            args = benchmark.argument_parser().parse_args(["--model-directory", str(model), "--output", str(output),
                                                           "--preflight-only"])
            with patch.object(benchmark.worker, "verify_model_files", return_value=manifest), \
                    patch.object(benchmark.worker, "load_model") as load:
                report = benchmark.run_benchmark(args)
                load.assert_not_called()
                self.assertEqual(report["status"], "preflight_only")
                self.assertFalse(report["pipelinePrecisionMeasured"])
                self.assertFalse(report["featureActivated"])
                for name in ("harness", "worker", "corpus", "installationManifest"):
                    self.assertEqual(len(report["provenance"][name]["sha256"]), 64)
                with self.assertRaises(FileExistsError):
                    benchmark.run_benchmark(args)

    def test_mock_run_persists_every_pair_and_keeps_uncertain_out_of_precision(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            model = base / "model"
            model.mkdir()
            manifest = {"model": "fixture", "revision": benchmark.worker.MODEL_REVISION, "files": []}
            (model / "installation.json").write_text(json.dumps(manifest))
            args = benchmark.argument_parser().parse_args(["--model-directory", str(model), "--output", str(base / "run")])
            with patch.object(benchmark.worker, "verify_model_files", return_value=manifest), \
                    patch.object(benchmark.worker, "load_model", return_value=fixture_model()) as load:
                report = benchmark.run_benchmark(args)
            load.assert_called_once_with(model, 2)
            records = [json.loads(line) for line in (base / "run/pairs.jsonl").read_text().splitlines()]
            self.assertEqual(len(records), 72)
            self.assertEqual(report["status"], "completed")
            self.assertEqual(report["results"]["heldout"]["labelledConfusion"], {"tp": 8, "fp": 16, "fn": 0, "tn": 0})
            self.assertEqual(report["results"]["heldout"]["uncertain"]["thresholdPassCases"], 8)
            self.assertFalse(report["featureActivated"])
            self.assertFalse(report["pipelinePrecisionMeasured"])
            self.assertEqual(json.loads((base / "run/summary.json").read_text())["status"], "completed")


if __name__ == "__main__":
    unittest.main()
