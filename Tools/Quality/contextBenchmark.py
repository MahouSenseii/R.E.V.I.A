"""Bounded, direct llama.cpp continuity probes; never changes Revia settings."""

import argparse
import csv
from datetime import datetime, timezone
import hashlib
import http.client
import io
import json
import os
from pathlib import Path
import socket
import subprocess
import threading
import time
import uuid


PROBES = (
    {
        "id": "late_decision_correction",
        "initial": "Source D1: The ORBIT workshop is on Thursday in Lisbon. Its budget is 4000 euros and its access code is LIME-47.",
        "correction": "Source D2: Correction to ORBIT: it is now on Tuesday in Oslo, the budget is 7200 euros, and the access code is CEDAR-82. These replace all four earlier values.",
        "question": 'Return the current ORBIT workshop decision as JSON with exactly day, city, budget_eur, access_code, source. Use an integer for budget_eur and the source ID for source.',
        "expected": {"day": "Tuesday", "city": "Oslo", "budget_eur": 7200, "access_code": "CEDAR-82", "source": "D2"},
    },
    {
        "id": "participant_attribution",
        "initial": "Source P1: Alice prefers mint tea. Bob prefers ginger tea. Alice's reading room is amber; Bob's reading room is cobalt.",
        "correction": "Source P2: Alice has switched her preferred tea to rooibos. Bob still prefers ginger. Neither person changed their reading room.",
        "question": 'Return JSON with exactly alice_tea, bob_tea, alice_room, bob_room, correction_source. Use only the established current facts.',
        "expected": {"alice_tea": "rooibos", "bob_tea": "ginger", "alice_room": "amber", "bob_room": "cobalt", "correction_source": "P2"},
    },
    {
        "id": "early_fact_and_unfinished_task",
        "initial": "Source T1: Project KESTREL stores its spare key in locker 618. The unresolved task is for Mara to check the north sensor on Monday. Project FABLE stores its key in locker 204.",
        "correction": "Source T2: For KESTREL, Mara finished checking the north sensor. The only remaining task is for Ivo to calibrate the west sensor on Friday. The spare key location has not changed.",
        "question": 'Return JSON for KESTREL with exactly spare_locker, remaining_owner, remaining_task, remaining_day, task_source. Use an integer for spare_locker and the full task phrase.',
        "expected": {"spare_locker": 618, "remaining_owner": "Ivo", "remaining_task": "calibrate the west sensor", "remaining_day": "Friday", "task_source": "T2"},
    },
)
SYSTEM = "Read this synthetic conversation carefully. Retain each person's facts separately. Later explicit corrections replace earlier claims only for the named subject. Answer the final request using the stated facts. Return only the requested JSON object, without commentary."
MAX_OUTPUT = 192


def messages_for(probe, distractors):
    messages = [{"role": "system", "content": SYSTEM}, {"role": "user", "content": probe["initial"]},
                {"role": "assistant", "content": "I have noted these facts and their source."}]
    correction_at = max(0, int(distractors * 0.8))
    for index in range(distractors + 1):
        if index == correction_at:
            messages.extend([{"role": "user", "content": probe["correction"]},
                             {"role": "assistant", "content": "I have noted the correction and retained facts that were not changed."}])
        if index < distractors:
            messages.extend([
                {"role": "user", "content": f"Archive note {index:04d}: unrelated inventory batch {index * 17 + 3} has {(index * 7) % 91 + 5} ceramic tiles. Its label is SAMPLE-{index:04d}; shelf {(index * 13) % 100} is reserved for catalog cards. A separate weather log recorded light clouds and mild wind. This archive entry makes no changes to any previously named person, project, workshop, decision or task."},
                {"role": "assistant", "content": f"Recorded archive note {index:04d} as unrelated inventory and weather information."},
            ])
    messages.append({"role": "user", "content": probe["question"]})
    return messages


def fit_probe(post, model, probe, context, reserve):
    target = min(int(context * 0.75), context - reserve - 128)
    best = None
    low, high = 0, context // 20
    while low <= high:
        count = (low + high) // 2
        messages = messages_for(probe, count)
        rendered = post("/apply-template", {"model": model, "messages": messages, "add_generation_prompt": True,
                                           "chat_template_kwargs": {"enable_thinking": False}})
        prompt = rendered.get("prompt")
        if not isinstance(prompt, str) or not prompt:
            raise RuntimeError("Backend did not supply the exact rendered prompt.")
        tokenized = post("/tokenize", {"model": model, "content": prompt, "add_special": True,
                                      "parse_special": True, "with_pieces": False})
        tokens = tokenized.get("tokens")
        if not isinstance(tokens, list) or not tokens or any(type(token) is not int for token in tokens):
            raise RuntimeError("Backend did not supply valid exact prompt tokens.")
        size = len(tokens)
        if size <= target:
            best = {"messages": messages, "renderedPrompt": prompt, "promptTokens": size,
                    "targetTokens": target, "distractorTurns": count}
            low = count + 1
        else:
            high = count - 1
    if best is None:
        raise RuntimeError("The fixed probe cannot fit with the output reserve.")
    return best


def grade(content, finish_reason, expected):
    try:
        answer = json.loads(content)
    except (json.JSONDecodeError, TypeError):
        return {"passed": False, "reason": "invalid_json", "correctFields": 0, "totalFields": len(expected)}
    correct = sum(type(answer.get(key)) is type(value) and answer.get(key) == value for key, value in expected.items()) if isinstance(answer, dict) else 0
    passed = finish_reason == "stop" and type(answer) is dict and set(answer) == set(expected) and correct == len(expected)
    return {"passed": passed, "reason": "exact_match" if passed else "mismatch_or_incomplete",
            "correctFields": correct, "totalFields": len(expected), "answer": answer}


def parse_gpu_csv(value):
    return [{"index": int(row[0]), "name": row[1].strip(), "totalMiB": int(row[2]), "usedMiB": int(row[3])}
            for row in csv.reader(io.StringIO(value)) if row]


def hidden_flags():
    return subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0


def gpu_snapshot():
    try:
        result = subprocess.run(["nvidia-smi", "--query-gpu=index,name,memory.total,memory.used", "--format=csv,noheader,nounits"],
                                capture_output=True, text=True, timeout=8, check=True, creationflags=hidden_flags())
        return {"devices": parse_gpu_csv(result.stdout)}
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        return {"unavailable": type(error).__name__}


class GpuSampler:
    def __init__(self):
        self.samples = []
        self.phase = "load"
        self.stopped = threading.Event()
        self.worker = threading.Thread(target=self.sample, daemon=True)

    def sample(self):
        while not self.stopped.is_set():
            phase = self.phase
            self.samples.append({"at": utc_now(), "phase": phase, **gpu_snapshot()})
            self.stopped.wait(2)

    def __enter__(self):
        self.worker.start()
        return self

    def __exit__(self, *_):
        self.stopped.set()
        self.worker.join(10)


def utc_now():
    return datetime.now(timezone.utc).isoformat()


def write_new_json(path, value):
    with path.open("x", encoding="utf-8", newline="\n") as output:
        json.dump(value, output, indent=2, ensure_ascii=False)
        output.write("\n")


def request_json(port, path, payload=None, timeout=180):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=timeout)
    try:
        data = json.dumps(payload, ensure_ascii=False).encode("utf-8") if payload is not None else None
        connection.request("POST" if payload is not None else "GET", path, body=data,
                           headers={"Content-Type": "application/json"})
        response = connection.getresponse()
        body = response.read(16 * 1024 * 1024 + 1)
        if len(body) > 16 * 1024 * 1024:
            raise RuntimeError("Backend response exceeded the bounded JSON size.")
        if response.status != 200:
            raise RuntimeError(f"Backend {path} returned HTTP {response.status}: {body[:500]!r}")
        result = json.loads(body)
        if not isinstance(result, dict):
            raise RuntimeError(f"Backend {path} returned a non-object response.")
        return result
    finally:
        connection.close()


def fresh_port():
    with socket.socket() as bound:
        bound.bind(("127.0.0.1", 0))
        return bound.getsockname()[1]


def stop_owned_process(process):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=10)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=10)


def server_arguments(server, model, context, port, alias, device):
    return [str(server), "--model", str(model), "--alias", alias, "--host", "127.0.0.1", "--port", str(port),
            "--ctx-size", str(context), "--parallel", "1", "--device", device, "--split-mode", "none",
            "--gpu-layers", "all", "--flash-attn", "on", "--threads", "8", "--threads-batch", "8",
            "--batch-size", "2048", "--ubatch-size", "512", "--cache-type-k", "f16", "--cache-type-v", "f16",
            "--cache-ram", "0", "--no-context-shift", "--reasoning", "off", "--fit", "off", "--offline"]


def wait_ready(process, port, alias, context):
    deadline = time.monotonic() + 150
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"Owned server exited before readiness, code {process.returncode}.")
        try:
            health = request_json(port, "/health", timeout=2)
            if health.get("status") == "ok":
                models = request_json(port, "/v1/models", timeout=5)
                if alias not in [item.get("id") for item in models.get("data", [])]:
                    raise RuntimeError("Loopback model identity did not match this owned server.")
                properties = request_json(port, "/props", timeout=5)
                actual = properties.get("default_generation_settings", {}).get("n_ctx")
                if actual != context or properties.get("total_slots") != 1:
                    raise RuntimeError(f"Backend context/slot mismatch: n_ctx={actual}, slots={properties.get('total_slots')}.")
                return properties
        except (ConnectionError, OSError, http.client.HTTPException):
            pass
        except RuntimeError as error:
            if "HTTP 503" not in str(error):
                raise
        time.sleep(0.25)
    raise RuntimeError("Owned server readiness timed out.")


def run_context(server, model, directory, context, device):
    directory.mkdir()
    port = fresh_port()
    alias = "revia-context-" + uuid.uuid4().hex
    arguments = server_arguments(server, model, context, port, alias, device)
    report = {"context": context, "startedAt": utc_now(), "arguments": arguments, "baselineGpu": gpu_snapshot(), "probes": []}
    write_new_json(directory / "launch.json", report)
    process = None
    # Ambient llama settings must not silently enable another model, remote bind, tools, or downloads.
    environment = {key: value for key, value in os.environ.items() if not key.startswith("LLAMA_ARG_")}
    with (directory / "server.log").open("xb") as log, GpuSampler() as sampler:
        try:
            started = time.perf_counter()
            process = subprocess.Popen(arguments, cwd=server.parent, env=environment, stdin=subprocess.DEVNULL,
                                       stdout=log, stderr=subprocess.STDOUT, creationflags=hidden_flags())
            report["ownedPid"] = process.pid
            properties = wait_ready(process, port, alias, context)
            report["loadSeconds"] = time.perf_counter() - started
            report["readyGpu"] = gpu_snapshot()
            write_new_json(directory / "properties.json", properties)
            for probe in PROBES:
                print(f"{context}: fitting {probe['id']}", flush=True)
                sampler.phase = "fit:" + probe["id"]
                fitted = fit_probe(lambda path, payload: request_json(port, path, payload), alias, probe, context, MAX_OUTPUT)
                request = {"model": alias, "messages": fitted["messages"], "temperature": 0, "seed": 42,
                           "max_tokens": MAX_OUTPUT, "stream": False, "cache_prompt": False,
                           "chat_template_kwargs": {"enable_thinking": False}, "response_format": {"type": "json_object"}}
                write_new_json(directory / (probe["id"] + ".request.json"), request)
                (directory / (probe["id"] + ".prompt.txt")).write_text(fitted["renderedPrompt"], encoding="utf-8")
                sampler.phase = "generate:" + probe["id"]
                started = time.perf_counter()
                response = request_json(port, "/v1/chat/completions", request)
                elapsed = time.perf_counter() - started
                write_new_json(directory / (probe["id"] + ".response.json"), response)
                choice = response["choices"][0]
                usage = response.get("usage", {})
                item = {"id": probe["id"], "promptTokens": fitted["promptTokens"], "targetTokens": fitted["targetTokens"],
                        "distractorTurns": fitted["distractorTurns"], "outputReserve": MAX_OUTPUT, "elapsedSeconds": elapsed,
                        "usage": usage, "backendTimings": response.get("timings"), "finishReason": choice.get("finish_reason"),
                        "grade": grade(choice["message"].get("content"), choice.get("finish_reason"), probe["expected"]),
                        "countMatchesUsage": fitted["promptTokens"] == usage.get("prompt_tokens")}
                report["probes"].append(item)
                write_new_json(directory / (probe["id"] + ".result.json"), item)
                print(f"{context}: {probe['id']} pass={item['grade']['passed']} tokens={fitted['promptTokens']} latency={elapsed:.3f}s", flush=True)
            report["status"] = "completed"
        except BaseException as error:
            report["status"] = "interrupted" if isinstance(error, KeyboardInterrupt) else "failed"
            report["error"] = str(error)
            raise
        finally:
            if process is not None:
                stop_owned_process(process)
                report["ownedProcessStopped"] = process.poll() is not None
            report["finishedAt"] = utc_now()
            sampler.stopped.set()
            sampler.worker.join(10)
            report["gpuSamples"] = sampler.samples
            report["gpuMeasurement"] = "Per-device system-wide nvidia-smi memory samples every 2 seconds; includes desktop and other processes, not attributed model allocation. Server diagnostics are retained in server.log; this build may not emit individual allocation sizes."
            write_new_json(directory / "results.json", report)
    return report


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(8 * 1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="New evidence directory; never replaces an existing directory.")
    parser.add_argument("--device", default="CUDA0", choices=("CUDA0", "CUDA1"))
    args = parser.parse_args()
    server, model = args.server.resolve(strict=True), args.model.resolve(strict=True)
    if not server.is_file() or not model.is_file():
        parser.error("--server and --model must be existing files")
    output = args.output.absolute()
    output.mkdir(parents=True, exist_ok=False)
    version = subprocess.run([str(server), "--version"], capture_output=True, text=True, timeout=15, creationflags=hidden_flags())
    run = {"schemaVersion": 1, "startedAt": utc_now(), "evidenceKind": "direct_backend_synthetic_probe",
           "model": str(model), "modelBytes": model.stat().st_size, "modelSha256": sha256_file(model),
           "server": str(server), "serverVersion": version.stdout + version.stderr, "contexts": [16384, 32768],
           "probesPerContext": len(PROBES), "concurrencyMeasured": False, "runtimeContinuityMeasured": False,
           "personalityMeasured": False, "settingsChanged": False}
    write_new_json(output / "run.json", run)
    reports = []
    try:
        for context in run["contexts"]:
            reports.append(run_context(server, model, output / str(context), context, args.device))
        write_new_json(output / "summary.json", {"finishedAt": utc_now(), "contexts": [
            {"context": report["context"], "passed": sum(item["grade"]["passed"] for item in report["probes"]),
             "total": len(report["probes"]), "probes": report["probes"], "loadSeconds": report["loadSeconds"]}
            for report in reports], "concurrencyMeasured": False, "runtimeContinuityMeasured": False, "personalityMeasured": False})
    except (OSError, RuntimeError, ValueError, KeyError, IndexError, KeyboardInterrupt, subprocess.SubprocessError) as error:
        print(f"Benchmark incomplete: {error}. Retained evidence: {output}", flush=True)
        return 2
    print(f"Evidence: {output}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
