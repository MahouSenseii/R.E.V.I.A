"""Turn exact recorded failures into dataset candidates for a reviewed export.

Optional and offline. `capture` reads recorded failures in one of two forms:

* a quality report with the traffic its observation proxy saved beside it (the report's
  .requests.json, .responses.json, .metadata.json and .provider.json);
* one exact serialized request and its raw response, from any harness that kept them.

Each selected failing turn is bound to the single request whose raw output is that turn's reply.
The messages the model actually received are kept exactly, and a corrections template is written
for the owner. `candidates` joins the owner's corrections and decisions to the captures in the
interchange format ReviaDatasetExport reads. Nothing here judges a reply or writes a correction.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import sys


VERSION = "stage-e-capture-v1"
CANDIDATES_SCHEMA = "revia-dataset-candidates-v1"
ROLES = ("system", "user", "assistant", "tool")
ISSUES = ("answer-coverage", "unsupported-claim", "memory-continuity", "missing-acceptance-evidence")
DECISIONS = ("pending", "accept", "rework", "reject", "need-evidence")
SETTINGS = ("model", "temperature", "top_p", "top_k", "min_p", "max_tokens", "presence_penalty", "frequency_penalty", "seed",
            "dry_multiplier", "dry_base", "dry_allowed_length", "dry_penalty_last_n", "chat_template_kwargs",
            "reasoning_budget_tokens", "stream", "stop", "response_format")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(data):
    return hashlib.sha256(data if isinstance(data, bytes) else data.encode("utf-8")).hexdigest()


def read_bytes(path, limit=1 << 30):
    with Path(path).open("rb") as stream:
        data = stream.read(limit + 1)
    require(len(data) <= limit, f"{path} exceeds its bound.")
    return data


def body_of(path):
    """A saved traffic record ({"body": "..."}) or the bare body itself."""
    data = read_bytes(path).decode("utf-8")
    try:
        value = json.loads(data)
    except json.JSONDecodeError:
        return data
    return value["body"] if isinstance(value, dict) and isinstance(value.get("body"), str) else data


def prompt_of(request_body):
    """The exact text messages the model received, or why they cannot be kept exactly."""
    request = json.loads(request_body)
    messages = request.get("messages")
    require(isinstance(messages, list) and messages, "The request has no messages.")
    prompt = []
    for message in messages:
        require(message.get("role") in ROLES, f"Unsupported message role {message.get('role')!r}.")
        require(isinstance(message.get("content"), str),
                "The prompt contains image or other non-text parts, which the text dataset cannot hold exactly.")
        require(not message.get("tool_calls"), "The prompt contains tool-call history, which the text dataset cannot hold exactly.")
        prompt.append({"role": message["role"], "content": message["content"]})
    require(prompt[-1]["role"] in ("user", "tool"), "The prompt does not end with the turn being answered.")
    settings = {key: request[key] for key in SETTINGS if key in request}
    return prompt, settings


def output_of(response_body):
    """The assistant text a response carried, streamed or not."""
    text = response_body.strip()
    if text.startswith("{"):
        message = json.loads(text)["choices"][0]["message"]
        require(not message.get("tool_calls"), "The reply is a tool call, which the text dataset cannot hold.")
        return message.get("content") or ""
    parts = []
    for line in text.splitlines():
        line = line.strip()
        if not line.startswith("data:"):
            continue
        payload = line[5:].strip()
        if payload == "[DONE]":
            break
        for choice in json.loads(payload).get("choices", []):
            delta = choice.get("delta", {})
            require(not delta.get("tool_calls"), "The reply is a tool call, which the text dataset cannot hold.")
            if isinstance(delta.get("content"), str):
                parts.append(delta["content"])
    return "".join(parts)


def record(prompt, settings, failed, source, provenance, context):
    require(failed, "The failed reply is empty; there is nothing to correct.")
    capture_id = sha(json.dumps(prompt, sort_keys=True) + "\n" + failed)[:16]
    provenance = {**provenance, "configurationSha256": sha(json.dumps(settings, sort_keys=True))}
    require(all(provenance.get(key) for key in ("model", "profileDigest", "templateSha256")),
            "The model, profile digest and chat template must all be known for an exact capture.")
    return {"captureId": capture_id, "source": source, "prompt": prompt, "failedOutput": failed, "failedOutputSha256": sha(failed),
            "requestSettings": settings, "provenance": provenance, "context": context}


def capture_traffic(report, selections):
    report = Path(report).absolute()
    paths = {name: Path(f"{report}.{name}.json") for name in ("requests", "responses", "metadata", "provider")}
    raw = {name: read_bytes(path) for name, path in paths.items()}
    report_bytes = read_bytes(report)
    metadata, provider = json.loads(raw["metadata"]), json.loads(raw["provider"])
    template = (provider.get("properties") or {}).get("chat_template")
    provenance = {"model": metadata.get("provider", ""), "profileDigest": metadata.get("profileDigest", ""),
                  "templateSha256": sha(template) if isinstance(template, str) and template else ""}
    responses = {item["requestIndex"]: item for item in json.loads(raw["responses"])}
    chats = []
    for item in json.loads(raw["requests"]):
        if not str(item.get("path", "")).endswith("/chat/completions"):
            continue
        response = responses.get(item["requestIndex"], {})
        if response.get("status") != 200 or not isinstance(response.get("body"), str):
            continue
        chats.append((item, response))
    cases = {entry["id"]: entry for entry in (json.loads(line) for line in report_bytes.decode("utf-8").splitlines() if line.strip())
             if entry.get("record") == "case"}
    source_base = {"report": str(report), "reportSha256": sha(report_bytes),
                   **{f"{name}Sha256": sha(data) for name, data in raw.items()}}
    captures, refused = [], []
    for selection in selections:
        match = re.fullmatch(r"(.+):(\d+)(?:@(\d+))?", selection)
        try:
            require(match, f"{selection!r} is not case:turn or case:turn@requestIndex.")
            case_id, turn_number, index = match.group(1), int(match.group(2)), match.group(3)
            require(case_id in cases and 1 <= turn_number <= len(cases[case_id].get("turns", [])), f"{selection} is not in the report.")
            turn = cases[case_id]["turns"][turn_number - 1]
            failed = turn.get("model_reply") or turn.get("rawReply") or ""
            require(failed, "The turn has no raw model reply to bind.")
            bound = []
            for item, response in chats:
                try:
                    if output_of(response["body"]) == failed:
                        bound.append((item, response))
                except (ValueError, KeyError, IndexError, json.JSONDecodeError):
                    continue
            if index is not None:
                bound = [pair for pair in bound if pair[0]["requestIndex"] == int(index)]
            require(len(bound) == 1, f"The turn's reply matches {len(bound)} recorded requests; select exactly one with @requestIndex."
                    if bound else "No recorded request produced the turn's reply.")
            item, _ = bound[0]
            prompt, settings = prompt_of(item["body"])
            captures.append(record(prompt, settings, failed, {**source_base, "kind": "traffic", "case": case_id, "turn": turn_number,
                                                              "requestIndex": item["requestIndex"], "requestBodySha256": sha(item["body"])},
                                   provenance, {"input": turn.get("input", ""), "failures": turn.get("failures", [])}))
        except (ValueError, KeyError, json.JSONDecodeError) as error:
            refused.append({"selection": selection, "reason": str(error)})
    return captures, refused


def capture_pair(arguments):
    request_body, response_body = body_of(arguments.request), body_of(arguments.response)
    prompt, settings = prompt_of(request_body)
    failed = output_of(response_body)
    source = {"kind": "pair", "request": str(Path(arguments.request).absolute()), "requestBodySha256": sha(request_body),
              "response": str(Path(arguments.response).absolute()), "responseBodySha256": sha(response_body)}
    provenance = {"model": arguments.model or settings.get("model", ""), "profileDigest": arguments.profile_digest or "",
                  "templateSha256": arguments.template_sha256 or ""}
    return [record(prompt, settings, failed, source, provenance, {"input": prompt[-1]["content"], "failures": []})], []


def command_capture(arguments):
    output = Path(arguments.output).absolute()
    require(not output.exists(), f"{output} already exists; a capture is never replaced.")
    if arguments.report:
        require(arguments.select, "Name each failing turn with --select case:turn.")
        captures, refused = capture_traffic(arguments.report, arguments.select)
    else:
        require(arguments.request and arguments.response, "Give --report, or both --request and --response.")
        captures, refused = capture_pair(arguments)
    require(captures, "Nothing was captured: " + "; ".join(item["reason"] for item in refused))
    require(len({item["captureId"] for item in captures}) == len(captures), "Two selections captured the same failure.")
    output.mkdir(parents=True)
    (output / "captures.jsonl").write_bytes("".join(json.dumps(item, sort_keys=True) + "\n" for item in captures).encode("utf-8"))
    template = {item["captureId"]: {"decision": "pending", "issue": "answer-coverage", "criterion": "", "evidence": "", "correction": "",
                                    "context": {**item["context"], "failedOutput": item["failedOutput"]}} for item in captures}
    (output / "corrections.template.json").write_text(json.dumps(template, indent=2) + "\n", encoding="utf-8")
    (output / "capture.json").write_text(json.dumps({"version": VERSION, "captured": len(captures), "refused": refused}, indent=2) + "\n",
                                         encoding="utf-8")
    return {"captured": len(captures), "refused": refused}


def command_candidates(arguments):
    directory = Path(arguments.captures)
    captures = [json.loads(line) for line in read_bytes(directory / "captures.jsonl").decode("utf-8").splitlines() if line.strip()]
    corrections = json.loads(read_bytes(arguments.corrections))
    output = Path(arguments.output)
    require(not output.exists(), f"{output} already exists; a candidate file is never replaced.")
    candidates, waiting = [], []
    for capture in captures:
        entry = corrections.get(capture["captureId"], {})
        decision = entry.get("decision", "pending")
        require(decision in DECISIONS, f"{capture['captureId']}: unknown decision {decision!r}.")
        if decision == "pending":
            waiting.append(capture["captureId"])
            continue
        require(entry.get("issue") in ISSUES, f"{capture['captureId']}: the issue must be one of {', '.join(ISSUES)}.")
        require(str(entry.get("criterion", "")).strip() and str(entry.get("evidence", "")).strip(),
                f"{capture['captureId']}: a decided capture needs the owner's criterion and evidence.")
        # The quality receipt's own bounds, so the exporter does not turn a long note into an exclusion.
        require(len(entry["criterion"].strip()) <= 1000 and len(entry["evidence"].strip()) <= 2048,
                f"{capture['captureId']}: keep the criterion within 1,000 characters and the evidence within 2,048.")
        correction = str(entry.get("correction", ""))
        require(decision != "accept" or (correction.strip() and correction != capture["failedOutput"]),
                f"{capture['captureId']}: an accepted capture needs a correction that differs from the failed reply.")
        source = capture["source"]
        session = (f"capture:{source['reportSha256'][:16]}:{source['case']}" if source["kind"] == "traffic"
                   else f"capture:{source['requestBodySha256'][:16]}")
        origin = {"companionId": arguments.companion, "sessionId": session, "generation": 1, "taskId": "", "attemptId": "",
                  "policyVersion": 0}
        provenance = capture["provenance"]
        # The exact request, not the report: every case in a campaign would otherwise share one source and
        # collapse into one partition. Turns of one case still group through their shared session.
        sources = [f"request:{source['requestBodySha256']}"]
        candidates.append({
            "exampleId": f"{arguments.prefix}:{capture['captureId']}", "origin": origin, "sourceIds": sources,
            "prompt": capture["prompt"], "response": correction, "targetOrigin": "reviewed-correction",
            "rejectedResponse": capture["failedOutput"],
            "judgment": {"source": "owner-judgment", "target": "conversation-turn", "issue": entry["issue"],
                         "judgmentId": f"owner:{capture['captureId']}", "sourceId": f"request:{capture['captureId']}",
                         "targetId": f"reply:{capture['captureId']}", "targetDigest": capture["failedOutputSha256"],
                         "criterion": entry["criterion"].strip(), "evidence": entry["evidence"].strip(),
                         "dependencies": {"loaded-profile-sha256": provenance["profileDigest"],
                                          "effective-llm-config-sha256": provenance["configurationSha256"],
                                          "capture-transformation": VERSION},
                         "correctsRecordId": "", "criterionSatisfied": False, "privateContextExcluded": False,
                         "audienceRevision": 1, "origin": origin},
            "review": decision, "modelId": provenance["model"], "templateId": f"chat-template-sha256:{provenance['templateSha256']}",
            "transformationVersion": VERSION})
    require(candidates, "No capture has a decision yet; fill in the corrections first.")
    output.write_text(json.dumps({"schema": CANDIDATES_SCHEMA, "candidates": candidates}, indent=2) + "\n", encoding="utf-8")
    return {"candidates": len(candidates), "waiting": waiting}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)
    capture = commands.add_parser("capture")
    capture.add_argument("--output", required=True, help="A new capture directory.")
    capture.add_argument("--report", help="A quality report whose observation-proxy traffic was saved beside it.")
    capture.add_argument("--select", action="append", help="A failing turn as case:turn, or case:turn@requestIndex.")
    capture.add_argument("--request", help="One exact serialized request (a saved traffic record or the bare body).")
    capture.add_argument("--response", help="Its raw response body.")
    capture.add_argument("--model")
    capture.add_argument("--profile-digest", dest="profile_digest")
    capture.add_argument("--template-sha256", dest="template_sha256")
    candidates = commands.add_parser("candidates")
    candidates.add_argument("--captures", required=True)
    candidates.add_argument("--corrections", required=True)
    candidates.add_argument("--output", required=True)
    candidates.add_argument("--prefix", default="stage-e")
    candidates.add_argument("--companion", default="revia")
    arguments = parser.parse_args(argv)
    try:
        result = {"capture": command_capture, "candidates": command_candidates}[arguments.command](arguments)
    except (ValueError, KeyError, OSError, json.JSONDecodeError) as error:
        print(json.dumps({"state": "failed", "error": f"{type(error).__name__}: {error}"}), file=sys.stderr)
        return 1
    print(json.dumps({"state": "complete", "command": arguments.command, **result}, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
