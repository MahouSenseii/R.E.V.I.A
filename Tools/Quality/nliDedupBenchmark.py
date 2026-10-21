"""Offline CPU NLI quality assay; fixed thresholds, no feature activation.

Scores the labelled corpus through the installed memory_nli worker functions.
This does not invoke C++ candidate, scope, revision or deterministic text gates,
and therefore measures model evidence only, never end-to-end pipeline precision.
"""
from __future__ import annotations

import argparse
from collections import Counter
from datetime import datetime, timezone
import hashlib
import importlib.metadata
import importlib.util
import json
import math
from pathlib import Path
import platform
import statistics
import time

ROOT = Path(__file__).resolve().parents[2]
WORKER_PATH = ROOT / "Tools/Workers/memory_nli.py"
_spec = importlib.util.spec_from_file_location("revia_nli_quality_worker", WORKER_PATH)
worker = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(worker)

MINIMUM_ENTAILMENT = 0.98
MAXIMUM_CONTRADICTION = 0.01
SPLITS = ("development", "heldout")
LABELS = ("equivalent", "distinct", "uncertain")


def file_record(path):
    path = Path(path)
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(4 * 1024 * 1024), b""):
            digest.update(block)
    return {"path": str(path.resolve()), "bytes": path.stat().st_size, "sha256": digest.hexdigest()}


def load_corpus(path):
    corpus = json.loads(Path(path).read_text(encoding="utf-8"))
    if not isinstance(corpus, dict) or corpus.get("version") != 1 or not isinstance(corpus.get("cases"), list):
        raise ValueError("Expected a version-1 labelled corpus.")
    if not 0 < len(corpus["cases"]) <= 1000:
        raise ValueError("The corpus must contain 1-1000 cases.")
    ids, pairs = set(), set()
    for row in corpus["cases"]:
        if not isinstance(row, dict) or any(not isinstance(row.get(key), str) or not row[key]
                                          for key in ("id", "split", "label", "category", "left", "right")):
            raise ValueError("Every case needs its ID, split, label, category and two texts.")
        if row["split"] not in SPLITS or row["label"] not in LABELS:
            raise ValueError("Unknown corpus split or equivalence label.")
        pair = tuple(sorted((row["left"], row["right"])))
        if row["id"] in ids or pair in pairs:
            raise ValueError("Duplicate IDs/pairs, including reversed or cross-split pairs, are not independent evidence.")
        if not worker.valid_text(row["left"]) or not worker.valid_text(row["right"]):
            raise ValueError("A corpus text exceeds the worker's exact input contract.")
        if "precheckMustReject" in row and type(row["precheckMustReject"]) is not bool:
            raise ValueError("precheckMustReject must be a declared boolean, never an observed model result.")
        ids.add(row["id"])
        pairs.add(pair)
    return corpus


class ObservedModel:
    """Records the unchanged six softmax outputs from the worker's one predict call."""
    def __init__(self, model):
        self.model = model
        self.tokenizer = model.tokenizer
        self.scores = None

    def predict(self, *args, **kwargs):
        self.scores = self.model.predict(*args, **kwargs)
        return self.scores


def threshold_pass(evidence):
    if evidence.get("model") != worker.MODEL_IDENTITY or evidence.get("truncated") is not False:
        return False
    for direction in ("forward", "reverse"):
        values = evidence[direction]
        entailment, contradiction = values["entailment"], values["contradiction"]
        if any(type(value) not in (int, float) or not math.isfinite(value) for value in (entailment, contradiction)):
            return False
        if not MINIMUM_ENTAILMENT <= entailment <= 1 or not 0 <= contradiction <= MAXIMUM_CONTRADICTION:
            return False
        if entailment + contradiction > 1.000001:
            return False
    return True


def run_cases(cases, model, on_record=None):
    records = []
    for row in cases:
        record = {key: row[key] for key in ("id", "split", "label", "category", "left", "right")}
        record.update(declaredPrecheckMustReject=row.get("precheckMustReject"), deterministicGuardInvoked=False)
        observed = ObservedModel(model)
        started = time.perf_counter()
        try:
            evidence = worker.score_request(observed, {"left": row["left"], "right": row["right"]})
            record.update(status="scored", workerEvidence=evidence, modelThresholdPass=threshold_pass(evidence),
                          rawProbabilities={direction: {worker.LABELS[index]: float(value)
                                                        for index, value in enumerate(scores)}
                                            for direction, scores in zip(("forward", "reverse"), observed.scores)})
        except MemoryError:
            raise
        except Exception as error:
            record.update(status="error", errorType=type(error).__name__, error=str(error)[:240])
        record["latencyMilliseconds"] = (time.perf_counter() - started) * 1000
        records.append(record)
        if on_record:
            on_record(record)
    return records


def summarize(records):
    summary = {}
    for split in SPLITS:
        selected = [row for row in records if row["split"] == split]
        scored = [row for row in selected if row["status"] == "scored"]
        confusion = {"tp": 0, "fp": 0, "fn": 0, "tn": 0}
        uncertain = [row for row in selected if row["label"] == "uncertain"]
        for row in scored:
            if row["label"] == "uncertain":
                continue
            positive, predicted = row["label"] == "equivalent", row["modelThresholdPass"]
            confusion["tp" if positive and predicted else "fn" if positive else "fp" if predicted else "tn"] += 1
        predicted = confusion["tp"] + confusion["fp"]
        equivalent = confusion["tp"] + confusion["fn"]
        times = sorted(row["latencyMilliseconds"] for row in scored)
        summary[split] = {
            "totalCases": len(selected), "scoredCases": len(scored), "labelledConfusion": confusion,
            "labelledScoredCases": sum(confusion.values()),
            "modelOnlyPrecision": confusion["tp"] / predicted if predicted else None,
            "modelOnlyRecall": confusion["tp"] / equivalent if equivalent else None,
            "falsePositiveIds": [row["id"] for row in scored if row["label"] == "distinct" and row["modelThresholdPass"]],
            "errorIds": [row["id"] for row in selected if row["status"] != "scored"],
            "uncertain": {"totalCases": len(uncertain),
                          "scoredCases": sum(row["status"] == "scored" for row in uncertain),
                          "thresholdPassCases": sum(row.get("modelThresholdPass") is True for row in uncertain),
                          "thresholdRejectCases": sum(row.get("modelThresholdPass") is False for row in uncertain),
                          "thresholdPassIds": [row["id"] for row in uncertain if row.get("modelThresholdPass") is True],
                          "errorIds": [row["id"] for row in uncertain if row["status"] != "scored"]},
            "scoredPairLatencyMilliseconds": {"mean": statistics.mean(times), "median": statistics.median(times),
                                              "p95": times[math.ceil(len(times) * 0.95) - 1], "maximum": times[-1]}
                                             if times else None,
        }
    return summary


def write_new_json(path, value):
    with Path(path).open("x", encoding="utf-8", newline="\n") as output:
        json.dump(value, output, indent=2, ensure_ascii=False, allow_nan=False)
        output.write("\n")


def versions():
    result = {"python": platform.python_version()}
    for package in ("torch", "transformers", "sentence-transformers", "tokenizers", "safetensors"):
        try:
            result[package] = importlib.metadata.version(package)
        except importlib.metadata.PackageNotFoundError:
            result[package] = None
    return result


def run_benchmark(args):
    corpus_path, model_directory = args.corpus.resolve(strict=True), args.model_directory.resolve(strict=True)
    corpus = load_corpus(corpus_path)
    manifest = worker.verify_model_files(model_directory)
    output = args.output.absolute()
    output.mkdir(parents=True, exist_ok=False)
    report = {
        "schemaVersion": 1, "startedAt": datetime.now(timezone.utc).isoformat(),
        "evidenceKind": "local_cpu_nli_model_quality", "modelIdentity": worker.MODEL_IDENTITY,
        "modelDirectory": str(model_directory), "device": "cpu", "threads": args.threads, "versions": versions(),
        "host": {"platform": platform.platform(), "machine": platform.machine(), "processor": platform.processor()},
        "thresholds": {"minimumEntailmentEachDirection": MINIMUM_ENTAILMENT,
                       "maximumContradictionEachDirection": MAXIMUM_CONTRADICTION,
                       "maximumEntailmentContradictionSum": 1.000001,
                       "tunedOnThisCorpus": False},
        "deterministicGuardInvoked": False, "candidateAndScopeGatesInvoked": False,
        "pipelinePrecisionMeasured": False, "featureActivated": False,
        "uncertainPolicy": "Reported separately; threshold passes on uncertain pairs require review and are not true positives.",
        "errorPolicy": "Errors are reported separately and excluded from the evaluated confusion denominator.",
        "corpusCounts": {split: dict(Counter(row["label"] for row in corpus["cases"] if row["split"] == split)) for split in SPLITS},
        "provenance": {"harness": file_record(Path(__file__)), "worker": file_record(WORKER_PATH),
                       "corpus": file_record(corpus_path), "installationManifest": file_record(model_directory / "installation.json"),
                       "verifiedModelFiles": manifest["files"],
                       "uninvokedDeterministicGateSources": [file_record(ROOT / path) for path in (
                           "Private/Memory/semanticDuplicate.cpp", "Private/Memory/memoryReconciliation.cpp")]},
    }
    write_new_json(output / "run.json", report)
    if args.preflight_only:
        report["status"] = "preflight_only"
        write_new_json(output / "summary.json", report)
        return report
    try:
        started = time.perf_counter()
        model = worker.load_model(model_directory, args.threads)
        report["modelLoadMilliseconds"] = (time.perf_counter() - started) * 1000
        started = time.perf_counter()
        worker.score_request(model, {"left": "The user enjoys tea.", "right": "The user likes tea."})
        report["warmupMilliseconds"] = (time.perf_counter() - started) * 1000
        with (output / "pairs.jsonl").open("x", encoding="utf-8", newline="\n") as pair_output:
            def save(record):
                pair_output.write(json.dumps(record, ensure_ascii=False, allow_nan=False) + "\n")
                pair_output.flush()
            records = run_cases(corpus["cases"], model, save)
        report["results"] = summarize(records)
        report["status"] = "completed" if all(row["status"] == "scored" for row in records) else "completed_with_errors"
    except (Exception, KeyboardInterrupt) as error:
        report.update(status="failed", errorType=type(error).__name__, error=str(error)[:240])
        report["finishedAt"] = datetime.now(timezone.utc).isoformat()
        write_new_json(output / "summary.json", report)
        raise
    report["finishedAt"] = datetime.now(timezone.utc).isoformat()
    write_new_json(output / "summary.json", report)
    return report


def argument_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model-directory", type=Path, required=True, help="Installed pinned local model; never downloaded here.")
    parser.add_argument("--corpus", type=Path, default=ROOT / "Tests/Data/semanticDuplicatePairs.json")
    parser.add_argument("--output", type=Path, required=True, help="New evidence directory; existing output is never replaced.")
    parser.add_argument("--threads", type=int, choices=range(1, 9), default=2)
    parser.add_argument("--preflight-only", action="store_true", help="Validate corpus and hash installed files without loading a model.")
    return parser


def main():
    args = argument_parser().parse_args()
    try:
        report = run_benchmark(args)
    except (Exception, KeyboardInterrupt) as error:
        print(f"NLI benchmark incomplete: {type(error).__name__}: {str(error)[:240]}", flush=True)
        return 2
    print(f"NLI benchmark {report['status']}: {args.output.absolute()}", flush=True)
    return 0 if report["status"] in ("completed", "preflight_only") else 2


if __name__ == "__main__":
    raise SystemExit(main())
