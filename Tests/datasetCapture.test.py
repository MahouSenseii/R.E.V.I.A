import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout


ROOT = Path(__file__).resolve().parents[1]
# The record ReviaLiveCaptureTests checks the C++ builder still produces, and the correction its
# store saves for it.
LIVE_FIXTURE = ROOT / "Tests" / "Fixture" / "liveCapture.json"
CORRECTION_FIXTURE = ROOT / "Tests" / "Fixture" / "liveCorrection.json"
TOOLS = [{"type": "function", "function": {"name": "calculator", "description": "Evaluate arithmetic.",
                                           "parameters": {"type": "object", "properties": {"expression": {"type": "string"}}}}}]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


CAPTURE = load("revia_dataset_capture", ROOT / "Tools" / "revia_dataset_capture.py")
WORKER = load("revia_training_worker", ROOT / "Tools" / "revia_training_worker.py")
EXPORT_TOOL = os.environ.get("REVIA_DATASET_EXPORT", "")


def sha(text):
    return hashlib.sha256(text.encode()).hexdigest()


def stream(text):
    """A streamed llama.cpp reply, split across server-sent events."""
    events = [{"choices": [{"delta": {"role": "assistant"}}]}]
    events += [{"choices": [{"delta": {"content": text[index:index + 7]}}]} for index in range(0, len(text), 7)]
    return "".join(f"data: {json.dumps(event)}\n\n" for event in events) + "data: [DONE]\n\n"


class Evidence:
    """A quality report with the traffic its observation proxy saves beside it."""

    def __init__(self, root, failing=8):
        self.root = Path(root)
        self.requests, self.responses, cases = [], [], []

        def chat(messages, reply, streamed=True, path="/v1/chat/completions", tools=None, thinking=False):
            index = len(self.requests)
            body = {"model": "revia", "messages": messages, "temperature": 0.8, "dry_multiplier": 0.8, "stream": streamed,
                    "chat_template_kwargs": {"enable_thinking": thinking}}
            if tools:
                body.update(tools=tools, tool_choice="auto")
            self.requests.append({"requestIndex": index, "method": "POST", "path": path, "body": json.dumps(body)})
            payload = stream(reply) if streamed else json.dumps({"choices": [{"message": {"role": "assistant", "content": reply}}]})
            self.responses.append({"requestIndex": index, "status": 200, "body": payload})
            return index

        system = {"role": "system", "content": "You are Revia."}
        greeting = [system, {"role": "user", "content": "Hi there."}]
        chat(greeting, "Hello!", streamed=False)
        chat([{"role": "user", "content": "Classify: Hi there."}], '{"category":"greeting"}', streamed=False)
        question = greeting + [{"role": "assistant", "content": "Hello!"}, {"role": "user", "content": "How did the poem make you feel?"}]
        self.feeling_request = chat(question, "The image shows a lake at dusk.")
        self.requests.append({"requestIndex": len(self.requests), "method": "POST", "path": "/v1/embeddings", "body": "{}"})
        self.responses.append({"requestIndex": len(self.requests) - 1, "status": 200, "body": "{}"})
        cases.append({"record": "case", "id": "feeling", "verdict": "fail", "turns": [
            {"input": "Hi there.", "reply": "Hello!", "model_reply": "Hello!", "failures": []},
            {"input": "How did the poem make you feel?", "reply": "The image shows a lake at dusk.",
             "model_reply": "The image shows a lake at dusk.", "failures": ["The personal question was not answered."]}]})
        for number in range(failing):
            code = f"def add{number}(a, b):\n    return a +"
            chat([system, {"role": "user", "content": f"Write add{number}(a, b) in Python."}], code)
            cases.append({"record": "case", "id": f"code-{number}", "verdict": "fail",
                          "turns": [{"input": f"Write add{number}(a, b).", "reply": code, "model_reply": code, "failures": ["Incomplete code."]}]})
        image = [system, {"role": "user", "content": [{"type": "text", "text": "Look."}, {"type": "image_url", "image_url": {"url": "data:"}}]}]
        chat(image, "A cat.")
        cases.append({"record": "case", "id": "image", "verdict": "fail", "turns": [{"input": "Look.", "reply": "A cat.", "model_reply": "A cat."}]})
        self.tooled_request = chat([system, {"role": "user", "content": "What is 6 times 7?"}], "It is 41.", tools=TOOLS)
        cases.append({"record": "case", "id": "tooled", "verdict": "fail",
                      "turns": [{"input": "What is 6 times 7?", "reply": "It is 41.", "model_reply": "It is 41.", "failures": ["Wrong."]}]})
        chat([system, {"role": "user", "content": "Think it over."}], "Thought it over.", thinking=True)
        cases.append({"record": "case", "id": "thinking", "verdict": "fail",
                      "turns": [{"input": "Think it over.", "reply": "Thought it over.", "model_reply": "Thought it over."}]})
        self.duplicates = [chat([system, {"role": "user", "content": f"Say same {index}."}], "Same.") for index in range(2)]
        cases.append({"record": "case", "id": "same", "verdict": "fail", "turns": [{"input": "Say same 1.", "reply": "Same.", "model_reply": "Same."}]})
        self.report = self.root / "native-e1.jsonl"
        self.report.write_bytes(("\n".join(json.dumps(item) for item in [{"record": "run"}] + cases) + "\n").encode())
        for name, value in (("requests", self.requests), ("responses", self.responses),
                            ("metadata", {"provider": "Qwen3.5-9B-Q4_K_M.gguf", "profileDigest": sha("profile")}),
                            ("provider", {"inventory": {}, "properties": {"chat_template": "{{ messages }}"}})):
            Path(f"{self.report}.{name}.json").write_text(json.dumps(value))

    def capture(self, output, *selections):
        arguments = ["capture", "--output", str(self.root / output), "--report", str(self.report)]
        for selection in selections:
            arguments += ["--select", selection]
        return run(arguments)


def run(arguments):
    out, err = io.StringIO(), io.StringIO()
    with redirect_stdout(out), redirect_stderr(err):
        code = CAPTURE.main(arguments)
    return code, json.loads(out.getvalue() or err.getvalue())


def captures(directory):
    return [json.loads(line) for line in (Path(directory) / "captures.jsonl").read_text().splitlines()]


class DatasetCaptureTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.evidence = Evidence(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def test_failing_turn_binds_to_its_exact_request(self):
        code, result = self.evidence.capture("first", "feeling:2")
        self.assertEqual((code, result["captured"], result["refused"]), (0, 1, []))
        [item] = captures(self.evidence.root / "first")
        request = json.loads(self.evidence.requests[self.evidence.feeling_request]["body"])
        self.assertEqual(item["prompt"], request["messages"], "The capture did not keep the exact messages the model received.")
        self.assertEqual((item["failedOutput"], item["source"]["requestIndex"]), ("The image shows a lake at dusk.", self.evidence.feeling_request))
        self.assertEqual(item["failedOutputSha256"], sha(item["failedOutput"]))
        self.assertEqual((item["provenance"]["model"], item["provenance"]["templateSha256"]),
                         ("Qwen3.5-9B-Q4_K_M.gguf", sha("{{ messages }}")))
        self.assertEqual(item["requestSettings"]["dry_multiplier"], 0.8)
        template = json.loads((self.evidence.root / "first" / "corrections.template.json").read_text())
        self.assertEqual(template[item["captureId"]]["decision"], "pending")
        self.assertEqual(self.evidence.capture("again", "feeling:2")[1]["captured"], 1)
        self.assertEqual(captures(self.evidence.root / "again")[0]["captureId"], item["captureId"], "A capture's identity is not stable.")
        self.assertEqual(self.evidence.capture("first", "feeling:2")[0], 1, "An existing capture directory was replaced.")

    def test_unbindable_or_inexact_turns_are_refused(self):
        code, result = self.evidence.capture("mixed", "image:1", "same:1", "missing:1", "code-0:1")
        self.assertEqual((code, result["captured"]), (0, 1))
        reasons = {item["selection"]: item["reason"] for item in result["refused"]}
        self.assertIn("non-text parts", reasons["image:1"])
        self.assertIn("matches 2 recorded requests", reasons["same:1"])
        self.assertIn("not in the report", reasons["missing:1"])
        code, result = self.evidence.capture("chosen", f"same:1@{self.evidence.duplicates[1]}")
        self.assertEqual((code, captures(self.evidence.root / "chosen")[0]["source"]["requestIndex"]), (0, self.evidence.duplicates[1]))
        code, result = self.evidence.capture("none", "image:1")
        self.assertEqual(code, 1)
        self.assertIn("Nothing was captured", result["error"])

    def test_offered_tools_are_kept_and_thinking_is_refused(self):
        code, result = self.evidence.capture("tools", "tooled:1", "thinking:1")
        self.assertEqual((code, result["captured"]), (0, 1))
        [item] = captures(self.evidence.root / "tools")
        self.assertEqual((item["tools"], item["requestSettings"]["tool_choice"]), (TOOLS, "auto"),
                         "The tools the template rendered into the prompt were not kept.")
        self.assertIn("thinking", result["refused"][0]["reason"])
        corrections = self.corrections(self.evidence.root / "tools", [("accept", "It is 42.")])
        run(["candidates", "--captures", str(self.evidence.root / "tools"), "--corrections", str(corrections),
             "--output", str(self.evidence.root / "tools" / "candidates.json")])
        [candidate] = json.loads((self.evidence.root / "tools" / "candidates.json").read_text())["candidates"]
        self.assertEqual(candidate["tools"], TOOLS)

    def test_pair_mode_needs_full_provenance(self):
        request = self.evidence.root / "request.json"
        request.write_text(json.dumps(self.evidence.requests[self.evidence.feeling_request]))
        response = self.evidence.root / "response.txt"
        response.write_text(stream("The image shows a lake at dusk."))
        base = ["capture", "--request", str(request), "--response", str(response)]
        code, result = run(base + ["--output", str(self.evidence.root / "pair-a")])
        self.assertEqual(code, 1)
        self.assertIn("must all be known", result["error"])
        code, _ = run(base + ["--output", str(self.evidence.root / "pair-b"), "--profile-digest", sha("profile"), "--template-sha256", sha("t")])
        self.assertEqual(code, 0)
        self.assertEqual(captures(self.evidence.root / "pair-b")[0]["source"]["kind"], "pair")

    def corrections(self, directory, decisions):
        template = json.loads((Path(directory) / "corrections.template.json").read_text())
        for capture, (decision, correction) in zip(captures(directory), decisions):
            template[capture["captureId"]].update(decision=decision, correction=correction,
                                                  criterion="Answers the question that was asked.", evidence="Owner review of Stage E.")
        path = Path(directory) / "corrections.json"
        path.write_text(json.dumps(template))
        return path

    def test_candidates_follow_the_owner_decisions(self):
        self.evidence.capture("decided", "feeling:2", "code-0:1", "code-1:1")
        directory = self.evidence.root / "decided"
        corrections = self.corrections(directory, [("accept", "It made me smile."), ("reject", ""), ("pending", "")])
        code, result = run(["candidates", "--captures", str(directory), "--corrections", str(corrections),
                            "--output", str(directory / "candidates.json")])
        self.assertEqual((code, result["candidates"], len(result["waiting"])), (0, 2, 1))
        document = json.loads((directory / "candidates.json").read_text())
        self.assertEqual(document["schema"], "revia-dataset-candidates-v1")
        accepted, rejected = document["candidates"]
        self.assertEqual((accepted["review"], rejected["review"]), ("accept", "reject"))
        self.assertEqual(accepted["judgment"]["targetDigest"], sha(accepted["rejectedResponse"]))
        self.assertEqual(accepted["judgment"]["origin"], accepted["origin"])
        self.assertEqual(set(accepted["judgment"]["dependencies"]),
                         {"loaded-profile-sha256", "effective-llm-config-sha256", "capture-transformation"})
        for decisions, message in (([("accept", "The image shows a lake at dusk.")], "differs from the failed reply"),
                                   ([("maybe", "x")], "unknown decision")):
            path = self.corrections(directory, decisions)
            code, result = run(["candidates", "--captures", str(directory), "--corrections", str(path),
                                "--output", str(directory / f"other-{len(message)}.json")])
            self.assertEqual(code, 1)
            self.assertIn(message, result["error"])

    def live(self, *records, corrected=False):
        """The companion's live capture directory, holding the C++ builder's record and, when asked,
        the correction its store saved for it."""
        directory = self.evidence.root / "Captures" / "live"
        directory.mkdir(parents=True, exist_ok=True)
        for record in records or (json.loads(LIVE_FIXTURE.read_text(encoding="utf-8")),):
            (directory / f"{record['captureId']}.json").write_text(json.dumps(record, indent=2))
        if corrected:
            saved = json.loads(CORRECTION_FIXTURE.read_text(encoding="utf-8"))
            (directory.parent / "corrections").mkdir(exist_ok=True)
            (directory.parent / "corrections" / f"{saved['captureId']}.json").write_text(json.dumps(saved, indent=2))
        return directory

    def live_corrections(self, directory, entry):
        number = len(list(directory.parent.glob("corrections-*")))
        written = directory.parent / f"template-{number}.json"
        code, _ = run(["template", "--captures", str(directory), "--output", str(written)])
        self.assertEqual(code, 0)
        template = json.loads(written.read_text())
        [key] = template
        template[key].update(entry)
        path = directory.parent / f"corrections-{number}.json"
        path.write_text(json.dumps(template))
        return path

    def test_live_capture_keeps_the_recorded_judgment(self):
        fixture = json.loads(LIVE_FIXTURE.read_text(encoding="utf-8"))
        directory = self.live()
        code, _ = run(["template", "--captures", str(directory), "--output", str(directory.parent / "template.json")])
        entry = json.loads((directory.parent / "template.json").read_text())[fixture["captureId"]]
        self.assertEqual((code, set(entry)), (0, {"decision", "correction", "context"}),
                         "A live capture's template asked for the judgment the owner already recorded.")
        code, result = run(["template", "--captures", str(directory), "--output", str(directory.parent / "template.json")])
        self.assertEqual(code, 1, "An existing corrections template was replaced.")
        self.assertEqual(entry["context"]["criterion"], fixture["judgment"]["criterion"])
        corrections = self.live_corrections(directory, {"decision": "accept", "correction": "Honestly, it made me smile."})
        output = directory.parent / "candidates.json"
        code, result = run(["candidates", "--captures", str(directory), "--corrections", str(corrections), "--output", str(output),
                            "--prefix", "live"])
        self.assertEqual((code, result["candidates"]), (0, 1), result)
        [candidate] = json.loads(output.read_text())["candidates"]
        self.assertEqual(candidate["judgment"], fixture["judgment"], "The owner's recorded judgment was not used verbatim.")
        self.assertEqual((candidate["origin"], candidate["sourceIds"], candidate["tools"], candidate["rejectedResponse"]),
                         (fixture["origin"], fixture["sources"], fixture["tools"], fixture["failedOutput"]))
        self.assertEqual((candidate["exampleId"], candidate["transformationVersion"]), (f"live:{fixture['captureId']}", "live-capture-v1"))

        refusals = [({"decision": "accept", "correction": "x", "criterion": "Something easier."}, "cannot be edited here"),
                    ({"decision": "accept", "correction": fixture["failedOutput"]}, "differs from the failed reply")]
        for entry, message in refusals:
            path = self.live_corrections(directory, entry)
            code, result = run(["candidates", "--captures", str(directory), "--corrections", str(path),
                                "--output", str(directory.parent / f"refused-{len(message)}.json")])
            self.assertEqual(code, 1)
            self.assertIn(message, result["error"])
        tampered = dict(fixture, failedOutput="A different reply.")
        other = self.evidence.root / "tampered" / "live"
        other.mkdir(parents=True)
        (other / f"{fixture['captureId']}.json").write_text(json.dumps(tampered))
        path = self.live_corrections(other, {"decision": "accept", "correction": "Honestly, it made me smile."})
        code, result = run(["candidates", "--captures", str(other), "--corrections", str(path), "--output", str(other.parent / "c.json")])
        self.assertEqual(code, 1)
        self.assertIn("not bound to the captured reply", result["error"])
        (other / "stray.json").write_text(json.dumps({"source": {"kind": "traffic"}}))
        code, result = run(["template", "--captures", str(other), "--output", str(other.parent / "t2.json")])
        self.assertEqual(code, 1)
        self.assertIn("not a live capture", result["error"])

    def test_saved_corrections_need_no_corrections_file(self):
        fixture = json.loads(LIVE_FIXTURE.read_text(encoding="utf-8"))
        saved = json.loads(CORRECTION_FIXTURE.read_text(encoding="utf-8"))
        directory = self.live()
        code, result = run(["candidates", "--captures", str(directory), "--output", str(directory.parent / "none.json")])
        self.assertEqual(code, 1)
        self.assertIn("No capture has a decision yet", result["error"])
        directory = self.live(corrected=True)
        output = directory.parent / "candidates.json"
        code, result = run(["candidates", "--captures", str(directory), "--output", str(output), "--prefix", "live"])
        self.assertEqual((code, result["candidates"]), (0, 1), result)
        [candidate] = json.loads(output.read_text())["candidates"]
        self.assertEqual((candidate["review"], candidate["response"], candidate["rejectedResponse"], candidate["judgment"]),
                         (saved["decision"], saved["correction"], fixture["failedOutput"], fixture["judgment"]),
                         "The saved decision and correction were not joined to the capture and its recorded judgment.")
        forged = dict(saved, failedOutputSha256=sha("Another reply."))
        (directory.parent / "corrections" / f"{saved['captureId']}.json").write_text(json.dumps(forged))
        code, result = run(["candidates", "--captures", str(directory), "--output", str(directory.parent / "forged.json")])
        self.assertEqual(code, 1)
        self.assertIn("is not the saved correction", result["error"])
        self.evidence.capture("recorded", "feeling:2")
        code, result = run(["candidates", "--captures", str(self.evidence.root / "recorded"),
                            "--output", str(self.evidence.root / "recorded" / "c.json")])
        self.assertEqual(code, 1)
        self.assertIn("need a --corrections file", result["error"])

    @unittest.skipUnless(EXPORT_TOOL and Path(EXPORT_TOOL).is_file(), "Set REVIA_DATASET_EXPORT to the built ReviaDatasetExport")
    def test_live_capture_exports_with_its_tools_and_is_withdrawn(self):
        fixture = json.loads(LIVE_FIXTURE.read_text(encoding="utf-8"))
        # Corrected in the companion's screen: nothing is edited by hand.
        directory = self.live(corrected=True)
        candidates = directory.parent / "candidates.json"
        run(["candidates", "--captures", str(directory), "--output", str(candidates), "--prefix", "live"])
        withdrawn = directory.parent / "withdrawn.json"
        withdrawn.write_text("[]")
        export = self.evidence.root / "live-export"
        completed = subprocess.run([EXPORT_TOOL, "--candidates", str(candidates), "--name", "live", "--salt", "live-1",
                                    "--output", str(export), "--withdrawn", str(withdrawn), "--validation-percent", "0",
                                    "--test-percent", "0"], capture_output=True, text=True, timeout=120)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(json.loads(completed.stdout)["eligible"], 1, completed.stdout)
        [line] = (export / "train.jsonl").read_text().splitlines()
        self.assertEqual(json.loads(line)["tools"], fixture["tools"], "The exported example lost the tools its prompt was rendered with.")
        manifest = (export / "manifest.json").read_bytes()
        job = {"dataset": {"directory": str(export), "manifestSha256": hashlib.sha256(manifest).hexdigest()},
               "withdrawnSourcesPath": str(withdrawn)}
        [record], _, _ = WORKER.verify_dataset(job)
        self.assertEqual(list(record["tools"][0]), ["type", "function"])
        self.assertEqual(list(record["tools"][0]["function"]), ["name", "description", "parameters"],
                         "The worker would not render the tools in the order the pinned server does.")
        # Deleting the chat in the companion lists it as withdrawn; the export is then refused.
        withdrawn.write_text(json.dumps(["conversation:conversation-1"]))
        with self.assertRaisesRegex(ValueError, "withdrawn since the export"):
            WORKER.verify_dataset(job)

    @unittest.skipUnless(EXPORT_TOOL and Path(EXPORT_TOOL).is_file(), "Set REVIA_DATASET_EXPORT to the built ReviaDatasetExport")
    def test_export_tool_makes_a_dataset_the_worker_reads(self):
        selections = ["feeling:2"] + [f"code-{number}:1" for number in range(8)]
        self.evidence.capture("all", *selections)
        directory = self.evidence.root / "all"
        decisions = [("reject", "")] + [("accept", f"def add{number}(a, b):\n    return a + b") for number in range(8)]
        corrections = self.corrections(directory, decisions)
        run(["candidates", "--captures", str(directory), "--corrections", str(corrections), "--output", str(directory / "candidates.json")])
        withdrawn = self.evidence.root / "withdrawn.json"
        withdrawn.write_text("[]")
        export = self.evidence.root / "export"
        completed = subprocess.run([EXPORT_TOOL, "--candidates", str(directory / "candidates.json"), "--name", "stage-e",
                                    "--salt", "stage-e-1", "--output", str(export), "--withdrawn", str(withdrawn)],
                                   capture_output=True, text=True, timeout=120)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        summary = json.loads(completed.stdout)
        self.assertEqual(summary["eligible"], 8)
        self.assertEqual([item["reason"] for item in summary["excluded"]], ["its review has not accepted it"])
        manifest = (export / "manifest.json").read_bytes()
        groups = {example["group"] for example in json.loads(manifest)["examples"] if example["eligible"]}
        self.assertEqual(len(groups), 8, "Independent cases from one campaign were merged into one group, so none can be held out.")
        job = {"dataset": {"directory": str(export), "manifestSha256": hashlib.sha256(manifest).hexdigest()},
               "withdrawnSourcesPath": str(withdrawn)}
        if summary["partitions"]["train"]:
            train, validation, identity = WORKER.verify_dataset(job)
            self.assertEqual(identity["trainExamples"], summary["partitions"]["train"])
            for record in train:
                self.assertEqual(record["messages"][-1]["role"], "assistant")
                self.assertNotEqual(record["messages"][-1]["content"], record["rejected"])
        source = json.loads(manifest)["examples"][0]["sources"][0]
        withdrawn.write_text(json.dumps([source]))
        with self.assertRaisesRegex(ValueError, "withdrawn since the export"):
            WORKER.verify_dataset(job)


if __name__ == "__main__":
    unittest.main(verbosity=2)
