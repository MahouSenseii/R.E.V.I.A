"""Evaluate a training candidate against its matched base from owned llama.cpp servers.

Optional and offline. `plan` freezes every input before anything is generated: the base and
adapter exports and their passed llama.cpp check, the dataset's held-out partition, the
golden set, answer-quality holdout and tool cases, the binaries, sampling and the decision
criteria. `run` serves one arm at a time, the quantized base and then the same file with
--lora. Each arm replays every held-out prompt exactly as it was captured, calls the tool
cases, and drives the existing ReviaAnswerGateLive and ReviaAnswerQualityLive against it.
`review` writes a blinded A/B sheet for the owner, who judges the held-out replies. `score`
applies the frozen criteria and decides accept, reject or incomplete. Nothing is activated.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
from pathlib import Path
import random
import shutil
import statistics
import subprocess
import sys


def _load_export():
    spec = importlib.util.spec_from_file_location("revia_adapter_export", Path(__file__).with_name("revia_adapter_export.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


EXPORT = _load_export()
WORKER = EXPORT.WORKER
require, digest, plain = WORKER.require, WORKER.digest, WORKER.plain
SCHEMA = "revia-adapter-evaluation-v1"
ARMS = ("base", "candidate")
DEFAULT_CRITERIA = {"minimumHeldOutCases": 5, "maximumRetentionRegressions": 1,
                    "maximumPrefillSlowdownPercent": 5.0, "maximumDecodeSlowdownPercent": 5.0}
INPUTS = ("server", "gate", "quality", "profile", "golden", "holdout", "toolCases")


def now():
    return datetime.now(timezone.utc).isoformat()


def read_json(path, limit=1 << 26):
    return json.loads(WORKER.read_bounded(Path(path), limit))


def read_lines(path, limit=1 << 28):
    return [json.loads(line) for line in WORKER.read_bounded(Path(path), limit).decode("utf-8").splitlines() if line.strip()]


def write_new(path, value):
    with Path(path).open("x", encoding="utf-8") as output:
        output.write(value if isinstance(value, str) else json.dumps(value, indent=2, sort_keys=True) + "\n")


def identity(path):
    path = plain(Path(path).absolute())
    require(path.is_file(), f"{path} is not a file.")
    return {"path": str(path), "sha256": digest(path)}


def tool(path):
    """A tool written in Python runs under this interpreter; anything else is executed directly."""
    return [sys.executable, "-I", "-B", path] if str(path).endswith(".py") else [path]


def held_out_cases(dataset):
    dataset = plain(Path(dataset))
    manifest_bytes = WORKER.read_bounded(dataset / "manifest.json", 1 << 26)
    manifest = json.loads(manifest_bytes)
    require(manifest.get("schema") == WORKER.DATASET_SCHEMA, "Unsupported dataset export schema.")
    test_bytes = WORKER.read_bounded(dataset / "test.jsonl", 1 << 28)
    require(hashlib.sha256(test_bytes).hexdigest() == manifest["files"]["test.jsonl"]["sha256"],
            "test.jsonl does not match its dataset manifest.")
    cases = [json.loads(line) for line in test_bytes.decode("utf-8").splitlines() if line]
    for case in cases:
        messages = case.get("messages")
        require(isinstance(messages, list) and len(messages) >= 2 and messages[-1].get("role") == "assistant",
                "A held-out example does not end with its reviewed reply.")
    return manifest_bytes, test_bytes, cases


def command_plan(arguments):
    final, staging = EXPORT.new_directory(arguments.output)
    try:
        base_dir, base_export = EXPORT.read_export(arguments.base_export, "base")
        adapter_dir, adapter_export = EXPORT.read_export(arguments.adapter_export, "adapter")
        require(adapter_export["base"] == base_export["base"], "The adapter export belongs to a different base export.")
        check = read_json(arguments.check, 1 << 22)
        require(check.get("kind") == "check" and check.get("passed") is True and check.get("adapter") is True and
                check.get("base") == base_export["base"], "The llama.cpp check for this base and adapter has not passed.")
        manifest_bytes, test_bytes, cases = held_out_cases(arguments.dataset)
        require(adapter_export["candidate"]["dataset"].get("manifestSha256") == hashlib.sha256(manifest_bytes).hexdigest(),
                "The candidate was trained on a different dataset export, so its held-out partition is not this one.")
        criteria = dict(DEFAULT_CRITERIA)
        for key in DEFAULT_CRITERIA:
            value = getattr(arguments, key, None)
            if value is not None:
                criteria[key] = value
        require(len(cases) >= criteria["minimumHeldOutCases"],
                f"The held-out partition has {len(cases)} cases; this plan requires {criteria['minimumHeldOutCases']}.")
        inputs = {name: identity(getattr(arguments, name)) for name in INPUTS}
        plan = {"schema": SCHEMA, "createdUtc": now(), "base": base_export["base"],
                "baseExport": {"directory": str(base_dir), "exportSha256": digest(base_dir / "export.json"),
                               "model": f"model-{base_export['quantization']}.gguf"},
                "adapterExport": {"directory": str(adapter_dir), "exportSha256": digest(adapter_dir / "export.json"),
                                  "candidate": adapter_export["candidate"]},
                "check": identity(arguments.check),
                "dataset": {"directory": str(plain(Path(arguments.dataset).absolute())),
                            "manifestSha256": hashlib.sha256(manifest_bytes).hexdigest(),
                            "testSha256": hashlib.sha256(test_bytes).hexdigest(), "heldOutCases": len(cases)},
                "inputs": inputs, "serverArguments": arguments.server_arg or [],
                "sampling": {"temperature": 0, "seed": arguments.seed, "maxTokens": arguments.max_tokens,
                             "contextSize": arguments.context_size, "enableThinking": False},
                "judgePort": arguments.judge_port, "criteria": criteria}
        write_new(staging / "plan.json", plan)
        staging.rename(final)
        return {"plan": str(final / "plan.json"), "planSha256": digest(final / "plan.json")}
    except BaseException:
        shutil.rmtree(staging, ignore_errors=True)
        raise


def verify_plan(directory):
    """Everything the plan named must still be exactly what it was when the criteria were fixed."""
    directory = plain(Path(directory).absolute())
    plan = read_json(directory / "plan.json", 1 << 22)
    require(plan.get("schema") == SCHEMA, "Unsupported evaluation plan.")
    for name, entry in plan["inputs"].items():
        require(digest(entry["path"]) == entry["sha256"], f"The plan's {name} changed after it was frozen.")
    require(digest(plan["check"]["path"]) == plan["check"]["sha256"], "The llama.cpp check changed after the plan.")
    for key in ("baseExport", "adapterExport"):
        exported = Path(plan[key]["directory"])
        require(digest(exported / "export.json") == plan[key]["exportSha256"], f"The {key} changed after the plan.")
    EXPORT.read_export(plan["baseExport"]["directory"], "base")
    EXPORT.read_export(plan["adapterExport"]["directory"], "adapter")
    manifest_bytes, test_bytes, cases = held_out_cases(plan["dataset"]["directory"])
    require(hashlib.sha256(manifest_bytes).hexdigest() == plan["dataset"]["manifestSha256"] and
            hashlib.sha256(test_bytes).hexdigest() == plan["dataset"]["testSha256"], "The dataset changed after the plan.")
    return directory, plan, cases


def judge_tool_call(expect, calls):
    """None when the calls meet the expectation, otherwise why not."""
    if expect["function"] is None:
        return None if not calls else "called a tool when none was needed"
    if len(calls) != 1:
        return f"made {len(calls)} tool calls instead of one"
    function = calls[0].get("function", {})
    if function.get("name") != expect["function"]:
        return f"called {function.get('name')!r} instead of {expect['function']!r}"
    try:
        arguments = function.get("arguments")
        arguments = json.loads(arguments) if isinstance(arguments, str) else arguments
    except json.JSONDecodeError:
        return "its arguments are not valid JSON"
    if not isinstance(arguments, dict):
        return "its arguments are not an object"
    text = {key: str(value).strip().lower() for key, value in arguments.items()}
    for key, value in expect.get("arguments", {}).items():
        if text.get(key) != str(value).strip().lower():
            return f"argument {key!r} is {arguments.get(key)!r}, not {value!r}"
    for key, value in expect.get("argumentsContain", {}).items():
        if str(value).lower() not in text.get(key, ""):
            return f"argument {key!r} does not contain {value!r}"
    return None


def complete(port, body):
    try:
        reply = EXPORT.request(port, "/v1/chat/completions", body, timeout=900)
    except ValueError as error:
        return {"error": str(error)}
    choice = reply["choices"][0]
    return {"message": choice.get("message", {}), "finish": choice.get("finish_reason"), "timings": reply.get("timings", {})}


def run_arm(plan, cases, staging, port, alias):
    sampling = plan["sampling"]
    common = {"temperature": 0, "seed": sampling["seed"], "max_tokens": sampling["maxTokens"],
              "chat_template_kwargs": {"enable_thinking": False}}
    lines = []
    for case in cases:
        # The exact captured prompt, without the reviewed reply that ends it.
        result = complete(port, {**common, "messages": case["messages"][:-1]})
        entry = {"id": case["id"], **({"error": result["error"]} if "error" in result else
                                     {"reply": result["message"].get("content") or "", "finish": result["finish"],
                                      "timings": result["timings"]})}
        lines.append(json.dumps(entry, sort_keys=True))
    write_new(staging / "held-out.jsonl", "\n".join(lines) + "\n")
    suite = read_json(plan["inputs"]["toolCases"]["path"])
    lines = []
    for case in suite["cases"]:
        result = complete(port, {**common, "max_tokens": 256, "messages": case["messages"], "tool_choice": "auto",
                                 "tools": [suite["tools"][name] for name in case["tools"]]})
        if "error" in result:
            entry = {"id": case["id"], "error": result["error"]}
        else:
            calls = result["message"].get("tool_calls") or []
            failure = judge_tool_call(case["expect"], calls)
            entry = {"id": case["id"], "passed": failure is None, "failure": failure, "calls": calls, "timings": result["timings"]}
        lines.append(json.dumps(entry, sort_keys=True))
    write_new(staging / "tools.jsonl", "\n".join(lines) + "\n")
    inputs = plan["inputs"]
    gate = tool(inputs["gate"]["path"]) + [inputs["profile"]["path"], inputs["golden"]["path"], str(port), "--output-dir",
                                           str(staging / "golden"), "--ignore-baseline", "--seed", str(sampling["seed"]),
                                           "--max-tokens", str(sampling["maxTokens"]), "--context-size", str(sampling["contextSize"]),
                                           "--expected-model", alias]
    gate += ["--judge-port", str(plan["judgePort"])] if plan.get("judgePort") else ["--no-judge"]
    gate_run = subprocess.run(gate, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=6 * 3600)
    quality = tool(inputs["quality"]["path"]) + [inputs["profile"]["path"], inputs["holdout"]["path"], str(port),
                                                 str(staging / "holdout.jsonl")]
    quality_run = subprocess.run(quality, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=6 * 3600)
    return {"gateExit": gate_run.returncode, "gateStderr": gate_run.stderr[-2000:],
            "qualityExit": quality_run.returncode, "qualityStderr": quality_run.stderr[-2000:]}


def command_run(arguments):
    directory, plan, cases = verify_plan(arguments.plan)
    target, staging = directory / arguments.arm, directory / f".{arguments.arm}.partial"
    require(not target.exists() and not staging.exists(), f"The {arguments.arm} arm already ran; an evaluation is never repeated in place.")
    staging.mkdir()
    try:
        model = Path(plan["baseExport"]["directory"]) / plan["baseExport"]["model"]
        lora = Path(plan["adapterExport"]["directory"]) / "adapter-f16.gguf" if arguments.arm == "candidate" else None
        alias = f"revia-eval-{arguments.arm}"
        started = now()
        server = EXPORT.Server(plan["inputs"]["server"]["path"], model, lora,
                               ["--alias", alias, "-c", str(plan["sampling"]["contextSize"]), *plan["serverArguments"]],
                               arguments.timeout)
        try:
            props = EXPORT.request(server.port, "/props")
            outcome = run_arm(plan, cases, staging, server.port, alias)
        finally:
            server.close()
        files = {path.relative_to(staging).as_posix(): digest(path) for path in sorted(staging.rglob("*")) if path.is_file()}
        receipt = {"arm": arguments.arm, "model": identity(model), "lora": identity(lora) if lora else None,
                   "build": props.get("build_info", ""), "startedUtc": started, "finishedUtc": now(), **outcome, "files": files}
        write_new(staging / "run.json", receipt)
        staging.rename(target)
        return receipt
    except BaseException:
        shutil.rmtree(staging, ignore_errors=True)
        raise


def arm_results(directory, arm):
    root, alias, problems = directory / arm, f"revia-eval-{arm}", []
    require((root / "run.json").exists(), f"The {arm} arm has not run.")

    def cases(path, suite, served):
        records = read_lines(path)
        model = served(next((record for record in records if record.get("record") == "run"), {}))
        if model != alias:
            # Verdicts about another model say nothing about this arm.
            problems.append(f"The {arm} arm's {suite} report names the model {model!r}, not {alias!r}.")
            return {}
        return {record["id"]: record for record in records if record.get("record") == "case"}

    golden_files = [path for path in sorted((root / "golden").glob("*.jsonl")) if path.name != "history.jsonl"] \
        if (root / "golden").exists() else []
    golden = cases(golden_files[-1], "golden set", lambda run: run.get("provenance", {}).get("model")) if golden_files else {}
    holdout = cases(root / "holdout.jsonl", "holdout", lambda run: run.get("model")) if (root / "holdout.jsonl").exists() else {}
    return {"run": read_json(root / "run.json"), "heldOut": {record["id"]: record for record in read_lines(root / "held-out.jsonl")},
            "tools": {record["id"]: record for record in read_lines(root / "tools.jsonl")}, "golden": golden, "holdout": holdout,
            "problems": problems}


def command_review(arguments):
    directory, plan, cases = verify_plan(arguments.plan)
    arms = {arm: arm_results(directory, arm) for arm in ARMS}
    shuffle = random.Random(plan["sampling"]["seed"])
    sheet, key, template = [], {}, {}
    for case in sorted(cases, key=lambda item: item["id"]):
        order = list(ARMS)
        shuffle.shuffle(order)
        replies = {}
        for label, arm in zip("AB", order):
            result = arms[arm]["heldOut"].get(case["id"], {})
            replies[label] = result.get("reply", f"(no reply: {result.get('error', 'missing')})")
        user = [message["content"] for message in case["messages"][:-1] if message.get("role") == "user"]
        sheet.append({"id": case["id"], "criterion": case.get("lineage", {}).get("criterion", ""),
                      "finalUserMessage": user[-1] if user else "", "conversation": case["messages"][:-1],
                      "reference": case["messages"][-1]["content"], "A": replies["A"], "B": replies["B"]})
        key[case["id"]] = dict(zip("AB", order))
        template[case["id"]] = {"A": None, "B": None}
    write_new(directory / "review.json", {"instructions": "Judge each of A and B against its criterion: pass or fail. The reference is "
                                                           "the reviewed reply; a different reply can still pass.", "cases": sheet})
    write_new(directory / "review-key.json", key)
    write_new(directory / "verdicts.template.json", template)
    return {"review": str(directory / "review.json"), "cases": len(sheet)}


def score(plan, arms, key, verdicts):
    criteria, results = plan["criteria"], {}
    if verdicts is None:
        results["heldOut"] = {"status": "incomplete", "reason": "The owner's verdicts are missing."}
    else:
        passed = {arm: set() for arm in ARMS}
        missing = []
        for case_id, labels in key.items():
            for label, arm in labels.items():
                verdict = (verdicts.get(case_id) or {}).get(label)
                if verdict not in ("pass", "fail"):
                    missing.append(case_id)
                elif verdict == "pass":
                    passed[arm].add(case_id)
        if missing:
            results["heldOut"] = {"status": "incomplete", "reason": f"{len(set(missing))} cases lack a pass or fail verdict."}
        else:
            new_failures = sorted(passed["base"] - passed["candidate"])
            improved = len(passed["candidate"]) > len(passed["base"])
            results["heldOut"] = {"status": "pass" if improved and not new_failures else "fail", "base": len(passed["base"]),
                                  "candidate": len(passed["candidate"]), "newFailures": new_failures}
    regressions, unavailable = [], []
    base, candidate = arms["base"], arms["candidate"]
    for case_id, record in base["golden"].items():
        other = candidate["golden"].get(case_id)
        if not record.get("available") or not other or not other.get("available"):
            unavailable.append("golden:" + case_id)
        elif record.get("deterministicPassed") and not other.get("deterministicPassed"):
            regressions.append("golden:" + case_id)
    for case_id, record in base["holdout"].items():
        other = candidate["holdout"].get(case_id, {}).get("verdict", "unjudged")
        if "unjudged" in (record.get("verdict"), other):
            unavailable.append("holdout:" + case_id)
        elif record.get("verdict") == "pass" and other == "fail":
            regressions.append("holdout:" + case_id)
    for case_id, record in base["tools"].items():
        other = candidate["tools"].get(case_id, {})
        if "error" in record or "error" in other:
            unavailable.append("tools:" + case_id)
        elif record.get("passed") and not other.get("passed"):
            regressions.append("tools:" + case_id)
    if not base["golden"] or not base["holdout"]:
        unavailable.append("A retention suite produced no results.")
    unavailable += [problem for arm in ARMS for problem in arms[arm].get("problems", [])]
    over = len(regressions) > criteria["maximumRetentionRegressions"]
    results["retention"] = {"status": "fail" if over else "incomplete" if unavailable else "pass",
                            "regressions": regressions, "unavailable": unavailable}
    latency = {}
    timed = {arm: {**{"held:" + key: value for key, value in arms[arm]["heldOut"].items()},
                   **{"tool:" + key: value for key, value in arms[arm]["tools"].items()}} for arm in ARMS}
    for field, limit, slower in (("prompt_ms", "maximumPrefillSlowdownPercent", lambda b, c: (c / b - 1) * 100),
                                 ("predicted_per_second", "maximumDecodeSlowdownPercent", lambda b, c: (1 - c / b) * 100)):
        # Only requests both arms completed, so the two medians describe the same prompts.
        matched = sorted(key for key, record in timed["base"].items()
                         if record.get("timings", {}).get(field) and timed["candidate"].get(key, {}).get("timings", {}).get(field))
        values = {arm: statistics.median(timed[arm][key]["timings"][field] for key in matched) if matched else None for arm in ARMS}
        latency[field] = {**values, "matchedRequests": len(matched), "limitPercent": criteria[limit],
                          "slowdownPercent": slower(values["base"], values["candidate"]) if matched else None}
    if any(entry["slowdownPercent"] is None for entry in latency.values()):
        results["latency"] = {"status": "incomplete", **latency}
    else:
        within = all(entry["slowdownPercent"] <= entry["limitPercent"] for entry in latency.values())
        results["latency"] = {"status": "pass" if within else "fail", **latency}
    results["llamaCppCheck"] = {"status": "pass", "check": plan["check"]["sha256"]}
    judged = {arm: [record["judge"]["score"] for record in arms[arm]["golden"].values() if record.get("judge", {}).get("available")]
              for arm in ARMS}
    statuses = [entry["status"] for entry in results.values()]
    decision = "reject" if "fail" in statuses else "incomplete" if "incomplete" in statuses else "accept"
    return {"decision": decision, "criteria": criteria, "results": results,
            "advisoryJudgeMeanScore": {arm: statistics.mean(scores) if scores else None for arm, scores in judged.items()}}


def command_score(arguments):
    directory, plan, _ = verify_plan(arguments.plan)
    arms = {arm: arm_results(directory, arm) for arm in ARMS}
    key = read_json(directory / "review-key.json") if (directory / "review-key.json").exists() else {}
    verdicts = read_json(arguments.verdicts) if arguments.verdicts else None
    require(verdicts is None or key, "Write the review sheet before scoring verdicts.")
    record = {"planSha256": digest(directory / "plan.json"), "scoredUtc": now(), "candidate": plan["adapterExport"]["candidate"],
              **score(plan, arms, key, verdicts)}
    if verdicts is not None:
        record["verdictsSha256"] = digest(arguments.verdicts)
    write_new(directory / "decision.json", record)
    return record


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)
    plan = commands.add_parser("plan")
    for name, flag in (("output", "--output"), ("base_export", "--base-export"), ("adapter_export", "--adapter-export"),
                       ("check", "--check"), ("dataset", "--dataset"), ("server", "--server"), ("gate", "--gate"),
                       ("quality", "--quality"), ("profile", "--profile")):
        plan.add_argument(flag, dest=name, required=True)
    plan.add_argument("--golden", default="Config/Evaluation/golden-set.jsonl")
    plan.add_argument("--holdout", default="Config/Evaluation/answer-quality-holdout.json")
    plan.add_argument("--tool-cases", dest="toolCases", default="Config/Evaluation/adapter-tool-cases.json")
    plan.add_argument("--server-arg", action="append", help="Extra llama-server argument, for example --server-arg=-ngl --server-arg=99.")
    plan.add_argument("--seed", type=int, default=20261031)
    plan.add_argument("--max-tokens", dest="max_tokens", type=int, default=512)
    plan.add_argument("--context-size", dest="context_size", type=int, default=8192)
    plan.add_argument("--judge-port", dest="judge_port", type=int, help="A llama-server for the golden set's model judge (advisory).")
    plan.add_argument("--minimum-held-out-cases", dest="minimumHeldOutCases", type=int)
    plan.add_argument("--maximum-retention-regressions", dest="maximumRetentionRegressions", type=int)
    plan.add_argument("--maximum-prefill-slowdown-percent", dest="maximumPrefillSlowdownPercent", type=float)
    plan.add_argument("--maximum-decode-slowdown-percent", dest="maximumDecodeSlowdownPercent", type=float)
    run = commands.add_parser("run")
    run.add_argument("--plan", required=True)
    run.add_argument("--arm", choices=ARMS, required=True)
    run.add_argument("--timeout", type=int, default=600)
    review = commands.add_parser("review")
    review.add_argument("--plan", required=True)
    scored = commands.add_parser("score")
    scored.add_argument("--plan", required=True)
    scored.add_argument("--verdicts")
    arguments = parser.parse_args(argv)
    try:
        record = {"plan": command_plan, "run": command_run, "review": command_review, "score": command_score}[arguments.command](arguments)
    except (ValueError, KeyError, subprocess.TimeoutExpired, OSError) as error:
        print(json.dumps({"state": "failed", "error": f"{type(error).__name__}: {error}"}), file=sys.stderr)
        return 1
    print(json.dumps({"state": "complete", "command": arguments.command, **({"decision": record["decision"]} if "decision" in record else {})}))
    if arguments.command == "score":
        return {"accept": 0, "reject": 1, "incomplete": 2}[record["decision"]]
    return 0


if __name__ == "__main__":
    sys.exit(main())
