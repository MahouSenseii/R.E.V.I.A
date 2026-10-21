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
import secrets
import subprocess
import tempfile
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
        "field_types": {"day": "string", "city": "string", "budget_eur": "integer", "access_code": "string", "source": "string"},
        "source_field": "source", "source_ids": ["D1", "D2"],
    },
    {
        "id": "participant_attribution",
        "initial": "Source P1: Alice prefers mint tea. Bob prefers ginger tea. Alice's reading room is amber; Bob's reading room is cobalt.",
        "correction": "Source P2: Alice has switched her preferred tea to rooibos. Bob still prefers ginger. Neither person changed their reading room.",
        "question": 'Return JSON with exactly alice_tea, bob_tea, alice_room, bob_room, correction_source. Use only the established current facts.',
        "expected": {"alice_tea": "rooibos", "bob_tea": "ginger", "alice_room": "amber", "bob_room": "cobalt", "correction_source": "P2"},
        "field_types": {"alice_tea": "string", "bob_tea": "string", "alice_room": "string", "bob_room": "string", "correction_source": "string"},
        "source_field": "correction_source", "source_ids": ["P1", "P2"],
    },
    {
        "id": "early_fact_and_unfinished_task",
        "initial": "Source T1: Project KESTREL stores its spare key in locker 618. The unresolved task is for Mara to check the north sensor on Monday. Project FABLE stores its key in locker 204.",
        "correction": "Source T2: For KESTREL, Mara finished checking the north sensor. The only remaining task is for Ivo to calibrate the west sensor on Friday. The spare key location has not changed.",
        "question": 'Return JSON for KESTREL with exactly spare_locker, remaining_owner, remaining_task, remaining_day, task_source. Use an integer for spare_locker and the full task phrase.',
        "expected": {"spare_locker": 618, "remaining_owner": "Ivo", "remaining_task": "calibrate the west sensor", "remaining_day": "Friday", "task_source": "T2"},
        "field_types": {"spare_locker": "integer", "remaining_owner": "string", "remaining_task": "string", "remaining_day": "string", "task_source": "string"},
        "source_field": "task_source", "source_ids": ["T1", "T2"],
    },
)
SYSTEM = "Read this synthetic conversation carefully. Retain each person's facts separately. Later explicit corrections replace earlier claims only for the named subject. Answer the final request using the stated facts. Return only the requested JSON object, without commentary."
MAX_OUTPUT = 192


def completion_request(model, probe, messages, response_format="json_schema"):
    request = {"model": model, "messages": messages, "temperature": 0, "seed": 42,
               "max_tokens": MAX_OUTPUT, "stream": False, "cache_prompt": False,
               "chat_template_kwargs": {"enable_thinking": False}, "response_format": {"type": response_format}}
    if response_format == "json_schema":
        # Types and the visible source catalog are task constraints. Expected answers
        # never participate: either old or new source IDs remain valid schema values.
        properties = {key: {"type": kind} for key, kind in probe["field_types"].items()}
        properties[probe["source_field"]]["enum"] = list(probe["source_ids"])
        request["response_format"]["json_schema"] = {"name": probe["id"], "strict": True, "schema": {
            "type": "object", "properties": properties, "required": list(properties), "additionalProperties": False}}
    elif response_format != "json_object":
        raise ValueError("Unsupported benchmark response format.")
    return request


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
    def unique_object(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError("Duplicate response key.")
            result[key] = value
        return result

    def reject_constant(_):
        raise ValueError("Nonstandard JSON number.")

    try:
        answer = json.loads(content, object_pairs_hook=unique_object, parse_constant=reject_constant)
    except (ValueError, TypeError):
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


class BackendHttpError(RuntimeError):
    def __init__(self, path, status):
        self.status = status
        # A server error body can echo an Authorization value. Never retain it.
        super().__init__(f"Backend {path} returned HTTP {status}.")


class EphemeralApiKey:
    """One operator-owned temporary credential, never inside an evidence directory."""
    def __init__(self):
        self._owned_path = None
        self.value = ""

    @property
    def path(self):
        return self._owned_path

    def __enter__(self):
        descriptor, filename = tempfile.mkstemp(prefix="revia-benchmark-key-", suffix=".txt")
        self._owned_path = Path(filename)
        self.value = secrets.token_hex(32)
        try:
            with os.fdopen(descriptor, "w", encoding="ascii", newline="\n") as output:
                output.write(self.value + "\n")
        except BaseException:
            self._owned_path.unlink(missing_ok=True)
            self.value = ""
            raise
        return self

    def __exit__(self, *_):
        # Remove exactly the file created by this owner, never a caller-selected path.
        try:
            if self._owned_path is not None:
                self._owned_path.unlink(missing_ok=True)
        finally:
            self.value = ""


def stream_redacted(source, destination, api_key):
    secret = api_key.encode("ascii")
    if not secret:
        raise ValueError("Log capture requires a nonempty owned credential.")
    pending = b""
    while True:
        block = source.read(4096)
        if not block:
            destination.write(pending.replace(secret, b"[REDACTED]"))
            destination.flush()
            return
        pending = (pending + block).replace(secret, b"[REDACTED]")
        # Keep enough trailing bytes to detect a credential split across reads.
        safe = max(0, len(pending) - len(secret) + 1)
        destination.write(pending[:safe])
        destination.flush()
        pending = pending[safe:]


class RedactingProcessLog:
    def __init__(self, path, api_key):
        self.path, self.api_key = path, api_key
        self.output = None
        self.worker = None
        self.failure = False

    def __enter__(self):
        self.output = self.path.open("xb")
        return self

    def capture(self, process):
        def pump():
            try:
                stream_redacted(process.stdout, self.output, self.api_key)
            except (OSError, ValueError):
                self.failure = True
        self.worker = threading.Thread(target=pump, name="benchmark-owned-log", daemon=True)
        self.worker.start()

    def __exit__(self, *_):
        if self.worker is not None:
            self.worker.join(10)
            if self.worker.is_alive():
                raise RuntimeError("Owned server log capture did not finish after shutdown.")
        self.output.close()
        if self.failure:
            raise RuntimeError("Owned server log capture failed.")


def require_api_key_file_support(server):
    result = subprocess.run([str(server), "--help"], capture_output=True, text=True, timeout=15,
                            creationflags=hidden_flags())
    if result.returncode != 0 or "--api-key-file" not in result.stdout + result.stderr:
        raise RuntimeError("The selected server does not advertise --api-key-file support.")


def request_json(port, path, payload=None, timeout=180, *, api_key=None):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=timeout)
    try:
        data = json.dumps(payload, ensure_ascii=False).encode("utf-8") if payload is not None else None
        headers = {"Content-Type": "application/json"}
        if api_key:
            headers["Authorization"] = "Bearer " + api_key
        connection.request("POST" if payload is not None else "GET", path, body=data, headers=headers)
        response = connection.getresponse()
        body = response.read(16 * 1024 * 1024 + 1)
        if len(body) > 16 * 1024 * 1024:
            raise RuntimeError("Backend response exceeded the bounded JSON size.")
        if response.status != 200:
            raise BackendHttpError(path, response.status)
        result = json.loads(body)
        if api_key and api_key in json.dumps(result, ensure_ascii=False):
            raise RuntimeError("Backend response echoed the owned credential; response was withheld.")
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


def server_arguments(server, model, context, port, alias, device, cache_type_k="f16", cache_type_v="f16", *, api_key_file=None):
    arguments = [str(server), "--model", str(model), "--alias", alias, "--host", "127.0.0.1", "--port", str(port),
            "--ctx-size", str(context), "--parallel", "1", "--device", device, "--split-mode", "none",
            "--gpu-layers", "all", "--flash-attn", "on", "--threads", "8", "--threads-batch", "8",
            "--batch-size", "2048", "--ubatch-size", "512", "--cache-type-k", cache_type_k, "--cache-type-v", cache_type_v,
            "--cache-ram", "0", "--no-context-shift", "--reasoning", "off", "--fit", "off", "--offline"]
    if api_key_file is not None:
        arguments.extend(["--api-key-file", str(api_key_file)])
    return arguments


def wait_ready(process, port, alias, context, *, api_key=None):
    deadline = time.monotonic() + 150
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"Owned server exited before readiness, code {process.returncode}.")
        try:
            health = request_json(port, "/health", timeout=2, api_key=api_key)
            if health.get("status") == "ok":
                if api_key:
                    # llama.cpp exposes /v1/models publicly even with keys enabled.
                    # /tokenize uses the same auth middleware as completion routes,
                    # but this tiny vocabulary-only request performs no inference.
                    auth_probe = {"content": "evaluation-auth-probe"}
                    wrong_key = secrets.token_hex(32)
                    while wrong_key == api_key:
                        wrong_key = secrets.token_hex(32)
                    try:
                        request_json(port, "/tokenize", auth_probe, timeout=5, api_key=wrong_key)
                    except BackendHttpError as error:
                        if error.status not in (401, 403):
                            raise
                    else:
                        raise RuntimeError("Owned endpoint did not reject an incorrect API key.")
                    tokenized = request_json(port, "/tokenize", auth_probe, timeout=5, api_key=api_key)
                    if not isinstance(tokenized.get("tokens"), list) or not tokenized["tokens"]:
                        raise RuntimeError("Owned endpoint did not accept the authenticated tokenizer probe.")
                models = request_json(port, "/v1/models", timeout=5, api_key=api_key)
                if [item.get("id") for item in models.get("data", [])] != [alias]:
                    raise RuntimeError("Loopback model identity did not match this owned server.")
                properties = request_json(port, "/props", timeout=5, api_key=api_key)
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


def run_context(server, model, directory, context, device, cache_type_k="f16", cache_type_v="f16", response_format="json_schema"):
    require_api_key_file_support(server)
    with EphemeralApiKey() as credential:
        return _run_context(server, model, directory, context, device, cache_type_k, cache_type_v, response_format, credential)


def _run_context(server, model, directory, context, device, cache_type_k, cache_type_v, response_format, credential):
    directory.mkdir()
    port = fresh_port()
    alias = "revia-context-" + uuid.uuid4().hex
    arguments = server_arguments(server, model, context, port, alias, device, cache_type_k, cache_type_v, api_key_file=credential.path)
    report = {"context": context, "startedAt": utc_now(), "arguments": arguments, "baselineGpu": gpu_snapshot(), "probes": [],
              "responseFormat": response_format, "cacheTypeK": cache_type_k, "cacheTypeV": cache_type_v,
              "responseContractVersion": 2 if response_format == "json_schema" else 1,
              "authentication": "ephemeral_api_key_file"}
    write_new_json(directory / "launch.json", report)
    process = None
    # Ambient llama settings must not silently enable another model, remote bind, tools, or downloads.
    environment = {key: value for key, value in os.environ.items() if not key.startswith("LLAMA_ARG_")}
    try:
        with RedactingProcessLog(directory / "server.log", credential.value) as log, GpuSampler() as sampler:
            try:
                started = time.perf_counter()
                process = subprocess.Popen(arguments, cwd=server.parent, env=environment, stdin=subprocess.DEVNULL,
                                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, creationflags=hidden_flags())
                log.capture(process)
                report["ownedPid"] = process.pid
                properties = wait_ready(process, port, alias, context, api_key=credential.value)
                report["loadSeconds"] = time.perf_counter() - started
                report["readyGpu"] = gpu_snapshot()
                write_new_json(directory / "properties.json", properties)
                for probe in PROBES:
                    print(f"{context}: fitting {probe['id']}", flush=True)
                    sampler.phase = "fit:" + probe["id"]
                    fitted = fit_probe(lambda path, payload: request_json(port, path, payload, api_key=credential.value),
                                       alias, probe, context, MAX_OUTPUT)
                    request = completion_request(alias, probe, fitted["messages"], response_format)
                    write_new_json(directory / (probe["id"] + ".request.json"), request)
                    (directory / (probe["id"] + ".prompt.txt")).write_text(fitted["renderedPrompt"], encoding="utf-8")
                    sampler.phase = "generate:" + probe["id"]
                    started = time.perf_counter()
                    response = request_json(port, "/v1/chat/completions", request, api_key=credential.value)
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
                report["error"] = str(error).replace(credential.value, "[REDACTED]")
                if isinstance(error, KeyboardInterrupt):
                    raise KeyboardInterrupt("Benchmark interrupted.") from None
                raise RuntimeError(report["error"]) from None
            finally:
                if process is not None:
                    stop_owned_process(process)
                    report["ownedProcessStopped"] = process.poll() is not None
                report["finishedAt"] = utc_now()
                sampler.stopped.set()
                sampler.worker.join(10)
                report["gpuSamples"] = sampler.samples
                report["gpuMeasurement"] = "Per-device system-wide nvidia-smi memory samples every 2 seconds; includes desktop and other processes, not attributed model allocation. Server diagnostics are retained in server.log; this build may not emit individual allocation sizes."
    except BaseException as error:
        # Log finalization is part of evidence completion, not a later best effort.
        report["status"] = "interrupted" if isinstance(error, KeyboardInterrupt) else "failed"
        report["error"] = str(error).replace(credential.value, "[REDACTED]")
        if isinstance(error, KeyboardInterrupt):
            raise KeyboardInterrupt("Benchmark interrupted.") from None
        raise RuntimeError(report["error"]) from None
    finally:
        report["finishedAt"] = utc_now()
        write_new_json(directory / "results.json", report)
    return report


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(8 * 1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def argument_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True, help="New evidence directory; never replaces an existing directory.")
    parser.add_argument("--device", default="CUDA0", choices=("CUDA0", "CUDA1"))
    parser.add_argument("--contexts", nargs="+", type=int, choices=(16384, 32768), default=[16384, 32768],
                        help="Context sizes to measure; defaults preserve the original paired sweep.")
    parser.add_argument("--cache-type-k", choices=("f16", "q8_0"), default="f16")
    parser.add_argument("--cache-type-v", choices=("f16", "q8_0"), default="f16")
    parser.add_argument("--response-format", choices=("json_schema", "json_object"), default="json_schema",
                        help="Strict declared shape, or the original syntax-only control. Facts and grading are unchanged.")
    return parser


def main():
    parser = argument_parser()
    args = parser.parse_args()
    if len(set(args.contexts)) != len(args.contexts):
        parser.error("--contexts cannot contain duplicates")
    server, model = args.server.resolve(strict=True), args.model.resolve(strict=True)
    if not server.is_file() or not model.is_file():
        parser.error("--server and --model must be existing files")
    output = args.output.absolute()
    output.mkdir(parents=True, exist_ok=False)
    version = subprocess.run([str(server), "--version"], capture_output=True, text=True, timeout=15, creationflags=hidden_flags())
    run = {"schemaVersion": 2, "startedAt": utc_now(), "evidenceKind": "direct_backend_synthetic_probe",
           "model": str(model), "modelBytes": model.stat().st_size, "modelSha256": sha256_file(model),
           "server": str(server), "serverVersion": version.stdout + version.stderr, "contexts": args.contexts,
           "responseFormat": args.response_format, "cacheTypeK": args.cache_type_k, "cacheTypeV": args.cache_type_v,
           "responseContractVersion": 2 if args.response_format == "json_schema" else 1,
           "harnessSha256": sha256_file(Path(__file__)),
           "probesPerContext": len(PROBES), "concurrencyMeasured": False, "runtimeContinuityMeasured": False,
           "personalityMeasured": False, "settingsChanged": False}
    write_new_json(output / "run.json", run)
    reports = []
    try:
        for context in run["contexts"]:
            reports.append(run_context(server, model, output / str(context), context, args.device,
                                       args.cache_type_k, args.cache_type_v, args.response_format))
        write_new_json(output / "summary.json", {"finishedAt": utc_now(), "contexts": [
            {"context": report["context"], "passed": sum(item["grade"]["passed"] for item in report["probes"]),
             "total": len(report["probes"]), "probes": report["probes"], "loadSeconds": report["loadSeconds"]}
            for report in reports], "concurrencyMeasured": False, "runtimeContinuityMeasured": False, "personalityMeasured": False,
            "responseFormat": args.response_format, "cacheTypeK": args.cache_type_k, "cacheTypeV": args.cache_type_v,
            "responseContractVersion": run["responseContractVersion"], "modelSha256": run["modelSha256"],
            "harnessSha256": run["harnessSha256"]})
    except (OSError, RuntimeError, ValueError, KeyError, IndexError, KeyboardInterrupt, subprocess.SubprocessError) as error:
        print(f"Benchmark incomplete: {error}. Retained evidence: {output}", flush=True)
        return 2
    print(f"Evidence: {output}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
