import hashlib
import http.server
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import tempfile
import threading
import unittest
from contextlib import redirect_stdout


ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


EVAL = load("revia_adapter_evaluation", ROOT / "Tools" / "revia_adapter_evaluation.py")
EXPORT_TESTS = load("revia_adapter_export_tests", ROOT / "Tests" / "reviaAdapterExport.test.py")
SMOKE = EXPORT_TESTS.SMOKE
RUNTIME = EXPORT_TESTS.RUNTIME
BASE = {"repository": "Qwen/Qwen3.5-4B", "revision": "8" * 40, "installManifestSha256": "f" * 64, "modelType": "qwen3_5_text",
        "architectures": [], "hiddenSize": 8, "layers": 2, "vocabSize": 32, "templateSha256": "a" * 64}

# Stand-ins for ReviaAnswerGateLive and ReviaAnswerQualityLive: the same arguments, result files and exit codes.
FAKE_GATE = r'''
import json, sys, urllib.request
from pathlib import Path
args = sys.argv[1:]
golden, port = args[1], args[2]
out = Path(args[args.index("--output-dir") + 1]); out.mkdir(parents=True)
model = args[args.index("--expected-model") + 1]
served = json.load(urllib.request.urlopen(f"http://127.0.0.1:{port}/v1/models", timeout=30))["data"][0]["id"]
if served != model:
    sys.exit(2)
(out / "arguments.json").write_text(json.dumps(args))
fail = set(json.loads(Path(__file__).with_name("behavior.json").read_text()).get(model, {}).get("golden", []))
ids = [json.loads(line)["id"] for line in open(golden, encoding="utf-8") if line.strip()][:4]
lines = [json.dumps({"record": "run"})] + [json.dumps({"record": "case", "id": case, "available": True,
    "deterministicPassed": case not in fail, "judge": {"available": False}}) for case in ids]
(out / "2026-10-31T00-00-00Z.jsonl").write_text("\n".join(lines) + "\n")
(out / "history.jsonl").write_text("{}\n")
'''
FAKE_QUALITY = r'''
import json, sys, urllib.request
from pathlib import Path
profile, corpus, port, report = sys.argv[1:5]
model = json.load(urllib.request.urlopen(f"http://127.0.0.1:{port}/v1/models", timeout=30))["data"][0]["id"]
fail = set(json.loads(Path(__file__).with_name("behavior.json").read_text()).get(model, {}).get("holdout", []))
ids = [case["id"] for case in json.load(open(corpus, encoding="utf-8"))["cases"]][:3]
lines = [json.dumps({"record": "run"})] + [json.dumps({"record": "case", "id": case,
    "verdict": "fail" if case in fail else "pass"}) for case in ids]
Path(report).write_text("\n".join(lines) + "\n")
sys.exit(1 if fail else 0)
'''


def sha(data):
    return hashlib.sha256(data).hexdigest()


def write_export(directory, record, files):
    directory.mkdir(parents=True)
    for name, data in files.items():
        (directory / name).write_bytes(data)
    record["files"] = {name: {"sha256": sha(data), "bytes": len(data)} for name, data in files.items()}
    (directory / "export.json").write_text(json.dumps(record))


class ChatServer:
    """A loopback model that records every request and answers with configurable latency."""

    def __init__(self, alias, prompt_ms, decode_rate):
        self.bodies = []
        owner = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *arguments):
                pass

            def reply(self, value):
                data = json.dumps(value).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def do_GET(self):
                self.reply({"data": [{"id": alias}]} if self.path == "/v1/models" else {"build_info": "fixture"})

            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                owner.bodies.append(body)
                message = {"role": "assistant", "content": "Reply to: " + body["messages"][-1]["content"]}
                if body.get("tools"):
                    names = [entry["function"]["name"] for entry in body["tools"]]
                    text = body["messages"][-1]["content"]
                    if "read_file" in names and ("notes.txt" in text or "todo.md" in text):
                        path = "notes.txt" if "notes.txt" in text else "todo.md"
                        message = {"role": "assistant", "content": "", "tool_calls": [
                            {"type": "function", "function": {"name": "read_file", "arguments": json.dumps({"path": path})}}]}
                self.reply({"choices": [{"message": message, "finish_reason": "stop"}],
                            "timings": {"prompt_ms": prompt_ms, "predicted_per_second": decode_rate}})

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.port = self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def close(self):
        self.server.shutdown()
        self.server.server_close()


class Campaign:
    def __init__(self, root, held_out=6, minimum=5):
        self.root = Path(root)
        dataset = self.root / "dataset"
        dataset.mkdir()
        lines = ""
        for index in range(held_out):
            messages = [{"role": "system", "content": "You are Revia."}, {"role": "user", "content": f"Question {index}?"},
                        {"role": "assistant", "content": f"Reviewed reply {index}."}]
            lines += json.dumps({"id": f"x{index}", "messages": messages, "lineage": {"criterion": "Answers the question asked."}}) + "\n"
        (dataset / "test.jsonl").write_bytes(lines.encode())
        manifest = json.dumps({"schema": "revia-dataset-v1", "files": {"test.jsonl": {"sha256": sha(lines.encode()), "examples": held_out}}}).encode()
        (dataset / "manifest.json").write_bytes(manifest)
        write_export(self.root / "base-export", {"kind": "base", "base": BASE, "quantization": "Q4_K_M"}, {"model-Q4_K_M.gguf": b"base"})
        candidate = {"jobId": "job-1", "candidateManifestSha256": "c" * 64, "configurationSha256": "d" * 64,
                     "dataset": {"manifestSha256": sha(manifest)}}
        write_export(self.root / "adapter-export", {"kind": "adapter", "base": BASE, "candidate": candidate}, {"adapter-f16.gguf": b"lora"})
        (self.root / "check.json").write_text(json.dumps({"kind": "check", "passed": True, "adapter": True, "base": BASE}))
        tools = self.root / "tools"
        tools.mkdir()
        (tools / "gate.py").write_text(FAKE_GATE)
        (tools / "quality.py").write_text(FAKE_QUALITY)
        (tools / "behavior.json").write_text("{}")
        (tools / "llama-server").write_bytes(b"server")
        self.minimum = minimum

    def arguments(self, **changes):
        values = {"output": str(self.root / "evaluation"), "base_export": str(self.root / "base-export"),
                  "adapter_export": str(self.root / "adapter-export"), "check": str(self.root / "check.json"),
                  "dataset": str(self.root / "dataset"), "server": str(self.root / "tools" / "llama-server"),
                  "gate": str(self.root / "tools" / "gate.py"), "quality": str(self.root / "tools" / "quality.py"),
                  "profile": str(ROOT / "Config" / "Profiles" / "revia.json"), "golden": str(ROOT / "Config" / "Evaluation" / "golden-set.jsonl"),
                  "holdout": str(ROOT / "Config" / "Evaluation" / "answer-quality-holdout.json"),
                  "toolCases": str(ROOT / "Config" / "Evaluation" / "adapter-tool-cases.json"), "server_arg": None, "seed": 7,
                  "max_tokens": 64, "context_size": 2048, "judge_port": None, "minimumHeldOutCases": self.minimum,
                  "maximumRetentionRegressions": None, "maximumPrefillSlowdownPercent": None, "maximumDecodeSlowdownPercent": None}
        values.update(changes)
        return type("Arguments", (), values)

    def run_arm(self, directory, arm, prompt_ms=100.0, decode_rate=50.0):
        server = ChatServer(f"revia-eval-{arm}", prompt_ms, decode_rate)
        try:
            _, plan, cases = EVAL.verify_plan(directory)
            staging = Path(directory) / arm
            staging.mkdir()
            outcome = EVAL.run_arm(plan, cases, staging, server.port, f"revia-eval-{arm}")
            (staging / "run.json").write_text(json.dumps({"arm": arm, **outcome}))
            return server.bodies, outcome
        finally:
            server.close()


def synthetic_arms(**changes):
    golden = {f"g{index}": {"id": f"g{index}", "available": True, "deterministicPassed": True, "judge": {"available": False}} for index in range(4)}
    holdout = {f"h{index}": {"id": f"h{index}", "verdict": "pass"} for index in range(3)}
    tools = {f"t{index}": {"id": f"t{index}", "passed": True, "timings": {"prompt_ms": 100.0, "predicted_per_second": 50.0}} for index in range(3)}
    held = {f"x{index}": {"id": f"x{index}", "reply": "r", "timings": {"prompt_ms": 100.0, "predicted_per_second": 50.0}} for index in range(5)}
    arms = {arm: {"golden": json.loads(json.dumps(golden)), "holdout": json.loads(json.dumps(holdout)),
                  "tools": json.loads(json.dumps(tools)), "heldOut": json.loads(json.dumps(held))} for arm in EVAL.ARMS}
    for path, value in changes.items():
        arm, suite, case, field = path.split(".")
        arms[arm][suite][case][field] = value
    return arms


class AdapterEvaluationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.campaign = Campaign(self.temporary.name)
        self.plan_record = {"criteria": dict(EVAL.DEFAULT_CRITERIA), "check": {"sha256": "e" * 64}}
        self.key = {f"x{index}": {"A": "base", "B": "candidate"} for index in range(5)}

    def tearDown(self):
        self.temporary.cleanup()

    def plan(self, **changes):
        return EVAL.command_plan(self.campaign.arguments(**changes))

    def test_plan_freezes_inputs_and_refuses_mismatches(self):
        result = self.plan()
        plan = json.loads(Path(result["plan"]).read_text())
        self.assertEqual(plan["criteria"]["maximumRetentionRegressions"], 1)
        self.assertEqual(plan["dataset"]["heldOutCases"], 6)
        self.assertEqual(set(plan["inputs"]), set(EVAL.INPUTS))
        tools = self.campaign.root / "tools"
        (tools / "gate.py").write_text(FAKE_GATE + "\n# changed\n")
        with self.assertRaisesRegex(ValueError, "gate changed after it was frozen"):
            EVAL.verify_plan(self.campaign.root / "evaluation")
        (tools / "gate.py").write_text(FAKE_GATE)
        for changes, message in (({"output": str(self.campaign.root / "e2"), "minimumHeldOutCases": 9}, "requires 9"),
                                 ({"output": str(self.campaign.root / "evaluation")}, "never replaced")):
            with self.assertRaisesRegex(ValueError, message):
                self.plan(**changes)
        check = self.campaign.root / "check.json"
        check.write_text(json.dumps({"kind": "check", "passed": False, "adapter": True, "base": BASE}))
        with self.assertRaisesRegex(ValueError, "has not passed"):
            self.plan(output=str(self.campaign.root / "e3"))
        check.write_text(json.dumps({"kind": "check", "passed": True, "adapter": True, "base": BASE}))
        other = json.loads((self.campaign.root / "adapter-export" / "export.json").read_text())
        other["candidate"]["dataset"]["manifestSha256"] = "0" * 64
        (self.campaign.root / "adapter-export" / "export.json").write_text(json.dumps(other))
        with self.assertRaisesRegex(ValueError, "different dataset export"):
            self.plan(output=str(self.campaign.root / "e4"))

    def test_tool_calls_are_judged_mechanically(self):
        call = lambda name, arguments: [{"function": {"name": name, "arguments": json.dumps(arguments)}}]
        judge = EVAL.judge_tool_call
        self.assertIsNone(judge({"function": "read_file", "arguments": {"path": "notes.txt"}}, call("read_file", {"path": " Notes.TXT"})))
        self.assertIsNone(judge({"function": "set_reminder", "argumentsContain": {"text": "stretch"}},
                                call("set_reminder", {"text": "Stretch at 3pm", "time": "15:00"})))
        self.assertIsNone(judge({"function": None}, []))
        for expect, calls, reason in (
                ({"function": "read_file", "arguments": {"path": "notes.txt"}}, call("calculate", {}), "instead of"),
                ({"function": "read_file", "arguments": {"path": "notes.txt"}}, call("read_file", {"path": "todo.md"}), "not 'notes.txt'"),
                ({"function": "read_file"}, [], "0 tool calls"),
                ({"function": None}, call("read_file", {"path": "x"}), "none was needed"),
                ({"function": "read_file"}, [{"function": {"name": "read_file", "arguments": "{oops"}}], "not valid JSON")):
            self.assertIn(reason, judge(expect, calls))

    def test_arm_replays_exact_prompts_and_drives_existing_tools(self):
        directory = Path(self.plan()["plan"]).parent
        bodies, outcome = self.campaign.run_arm(directory, "base")
        self.assertEqual((outcome["gateExit"], outcome["qualityExit"]), (0, 0), outcome)
        _, _, cases = EVAL.verify_plan(directory)
        replays = [body for body in bodies if not body.get("tools")]
        self.assertEqual([body["messages"] for body in replays], [case["messages"][:-1] for case in cases],
                         "A held-out prompt was not replayed exactly as captured, without its reviewed reply.")
        for body in bodies:
            self.assertEqual((body["temperature"], body["seed"], body["chat_template_kwargs"]), (0, 7, {"enable_thinking": False}))
        gate = json.loads((directory / "base" / "golden" / "arguments.json").read_text())
        self.assertIn("--no-judge", gate)
        self.assertEqual(gate[gate.index("--expected-model") + 1], "revia-eval-base")
        tools = {record["id"]: record for record in EVAL.read_lines(directory / "base" / "tools.jsonl")}
        self.assertTrue(tools["tool-read-named-file"]["passed"] and tools["tool-none-needed"]["passed"])
        self.assertFalse(tools["tool-calculation"]["passed"])

    def test_review_is_blinded_and_score_reaches_a_decision(self):
        directory = Path(self.plan()["plan"]).parent
        self.campaign.run_arm(directory, "base")
        (self.campaign.root / "tools" / "behavior.json").write_text(json.dumps({"revia-eval-candidate": {"golden": ["code-001"]}}))
        self.campaign.run_arm(directory, "candidate")
        with redirect_stdout(io.StringIO()):
            self.assertEqual(EVAL.main(["review", "--plan", str(directory)]), 0)
        sheet = (directory / "review.json").read_text()
        self.assertNotIn("revia-eval", sheet)
        self.assertNotIn("candidate\"", sheet)
        key = json.loads((directory / "review-key.json").read_text())
        verdicts = {case: {label: ("pass" if arm == "candidate" or case != "x0" else "fail") for label, arm in labels.items()}
                    for case, labels in key.items()}
        (self.campaign.root / "verdicts.json").write_text(json.dumps(verdicts))
        with redirect_stdout(io.StringIO()):
            code = EVAL.main(["score", "--plan", str(directory), "--verdicts", str(self.campaign.root / "verdicts.json")])
        decision = json.loads((directory / "decision.json").read_text())
        self.assertEqual((code, decision["decision"]), (0, "accept"), json.dumps(decision["results"], indent=1))
        self.assertEqual(decision["results"]["retention"]["regressions"], ["golden:code-001"])
        with redirect_stdout(io.StringIO()):
            self.assertEqual(EVAL.main(["score", "--plan", str(directory), "--verdicts", str(self.campaign.root / "verdicts.json")]), 1)

    def test_score_rejects_or_waits_on_the_frozen_criteria(self):
        verdicts = {case: {"A": "pass" if case != "x0" else "fail", "B": "pass"} for case in self.key}
        accept = EVAL.score(self.plan_record, synthetic_arms(), self.key, verdicts)
        self.assertEqual(accept["decision"], "accept")
        cases = [
            ("incomplete", synthetic_arms(), None, "heldOut"),
            ("reject", synthetic_arms(), {case: {"A": "pass", "B": "pass"} for case in self.key}, "heldOut"),
            ("reject", synthetic_arms(), {**verdicts, "x1": {"A": "pass", "B": "fail"}}, "heldOut"),
            ("reject", synthetic_arms(**{"candidate.golden.g0.deterministicPassed": False,
                                          "candidate.tools.t1.passed": False}), verdicts, "retention"),
            ("incomplete", synthetic_arms(**{"candidate.holdout.h2.verdict": "unjudged"}), verdicts, "retention"),
        ]
        # More passes do not excuse a case the base passed and the candidate now fails.
        traded = {"x0": {"A": "fail", "B": "pass"}, "x1": {"A": "pass", "B": "fail"}, "x2": {"A": "pass", "B": "pass"},
                  "x3": {"A": "fail", "B": "pass"}, "x4": {"A": "fail", "B": "pass"}}
        cases.append(("reject", synthetic_arms(), traded, "heldOut"))
        slow = synthetic_arms()
        for record in slow["candidate"]["heldOut"].values():
            record["timings"]["prompt_ms"] = 120.0
        cases.append(("reject", slow, verdicts, "latency"))
        for expected, arms, verdict, criterion in cases:
            with self.subTest(expected=expected, criterion=criterion):
                result = EVAL.score(self.plan_record, arms, self.key, verdict)
                self.assertEqual(result["decision"], expected, json.dumps(result["results"][criterion], indent=1))
                self.assertNotEqual(result["results"][criterion]["status"], "pass")

    def test_latency_compares_only_requests_both_arms_completed(self):
        arms = synthetic_arms()
        for record in arms["base"]["heldOut"].values():
            record["timings"]["prompt_ms"] = 50.0
        for case, record in arms["candidate"]["heldOut"].items():
            arms["candidate"]["heldOut"][case] = {"id": case, "error": "the server rejected the output"}
        verdicts = {case: {"A": "pass" if case != "x0" else "fail", "B": "pass"} for case in self.key}
        latency = EVAL.score(self.plan_record, arms, self.key, verdicts)["results"]["latency"]
        self.assertEqual((latency["status"], latency["prompt_ms"]["matchedRequests"]), ("pass", 3), json.dumps(latency, indent=1))

    def test_an_arm_never_reruns_in_place(self):
        directory = Path(self.plan()["plan"]).parent
        (directory / "base").mkdir()
        with self.assertRaisesRegex(ValueError, "already ran"):
            EVAL.command_run(type("Arguments", (), {"plan": str(directory), "arm": "base", "timeout": 30}))

    @unittest.skipUnless(all(SMOKE.values()) and RUNTIME,
                         "Needs the training runtime and the tiny model, llama.cpp source, binaries and converter Python")
    def test_both_arms_on_a_real_llama_server(self):
        smoke = Path(self.temporary.name) / "smoke"
        smoke.mkdir()
        test = EXPORT_TESTS.AdapterExportTests("test_tiny_model_roundtrip_through_llama_cpp")
        test.root = Path(self.temporary.name)
        test.test_tiny_model_roundtrip_through_llama_cpp()
        suffix = ".exe" if os.name == "nt" else ""
        arguments = self.campaign.arguments(
            output=str(smoke / "evaluation"), base_export=str(smoke / "base-export"), adapter_export=str(smoke / "adapter-export"),
            check=str(smoke / "check.json"), dataset=str(smoke / "dataset"), minimumHeldOutCases=1,
            server=str(Path(SMOKE["REVIA_LLAMA_BIN_DIR"]) / f"llama-server{suffix}"))
        directory = Path(EVAL.command_plan(arguments)["plan"]).parent
        for arm in EVAL.ARMS:
            receipt = EVAL.command_run(type("Arguments", (), {"plan": str(directory), "arm": arm, "timeout": 300}))
            self.assertEqual((receipt["gateExit"], receipt["qualityExit"]), (0, 0), receipt)
            held = EVAL.read_lines(directory / arm / "held-out.jsonl")
            self.assertEqual([record["id"] for record in held], ["x0"])
        EVAL.command_review(type("Arguments", (), {"plan": str(directory)}))
        record = EVAL.command_score(type("Arguments", (), {"plan": str(directory), "verdicts": None}))
        # Without the owner's verdicts the candidate can never be accepted; a measured failure, such as the LoRA's
        # overhead on a tiny CPU model, may still reject it.
        statuses = {name: entry["status"] for name, entry in record["results"].items()}
        self.assertEqual(statuses["heldOut"], "incomplete", statuses)
        self.assertNotEqual(record["decision"], "accept", statuses)
        self.assertTrue(all(entry["slowdownPercent"] is not None for name, entry in record["results"]["latency"].items()
                            if isinstance(entry, dict)), record["results"]["latency"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
