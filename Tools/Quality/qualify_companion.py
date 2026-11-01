"""Finite fixture qualification. No model or external service is launched."""
import argparse
from collections import Counter
import hashlib
import http.server
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import threading
import time
import uuid

sys.dont_write_bytecode = True
MODEL = "qual1-synthetic-provider-v1"
INITIAL = "I use Cedar as Project Juniper's label."
CORRECTION = "Actually, I now use Maple as Project Juniper's label."
MEMORY_MARKER = "Retrieved long-term memory and sourced research summaries."
SENTINELS = ("Cedar", "Maple", "QualificationOwner")
FAULTS = ("none", "empty-state", "rejected-correction", "failed-save", "retrieval-disabled", "interrupt-before-receipt")
STATES = ("passed", "failed", "blocked", "skipped", "not_run", "interrupted")
CHILD_RECEIPT_FIELDS = {
    "schema_version", "scope", "real_model", "status", "checks", "durable_receipt", "pid", "phase", "fault", "run_id", "runtime_root",
    "configuration_before_start", "configuration_effective", "credential_binding", "production_start", "companion_id",
    "default_current_relationship", "relationship_loaded", "working_context_before_introduction", "relationship", "revision_count",
    "memory_after_write", "answer", "public_answer", "private_rows_before_public", "private_rows_after_public", "private_control_answer", "error"}


class Blocked(ValueError):
    pass


def unique_json(raw):
    def distinct(pairs):
        obj = {}
        for key, value in pairs:
            if key in obj:
                raise ValueError("Duplicate JSON field")
            obj[key] = value
        return obj
    return json.loads(raw, object_pairs_hook=distinct)


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def identity(value):
    return sha(json.dumps(value, sort_keys=True, separators=(",", ":")).encode())


def source_identity():
    source = Path(__file__).resolve().parents[2]
    files = {file.relative_to(source).as_posix(): sha(file.read_bytes()) for file in source.rglob("*")
             if file.is_file() and not {"build", ".git", "__pycache__"}.intersection(file.relative_to(source).parts)}
    return {"pin": "3514b38", "reviewed_base_sha256": "e32853f740e9b48a3f172035e273020fbe3c55a125be2e16e6c4b5d6ea6f9bd8",
            "manifest_sha256": identity(files), "files": files}


def redact(value, secret):
    if isinstance(value, str):
        return value.replace(secret, "[REDACTED]") if secret else value
    if isinstance(value, list):
        return [redact(item, secret) for item in value]
    if isinstance(value, dict):
        return {redact(key, secret): redact(item, secret) for key, item in value.items()}
    return value


def safe_path(path):
    path = Path(os.path.abspath(path)) if Path(path).is_absolute() else Path(path)
    if not path.is_absolute():
        raise Blocked("Absolute paths required")
    for ancestor in (path, *path.parents):
        if ancestor.is_symlink() or (ancestor.exists() and getattr(ancestor.lstat(), "st_file_attributes", 0) & 0x400):
            raise Blocked("Redirected paths are refused")
    return path


def fresh_paths(runtime, report):
    runtime, report = safe_path(runtime), safe_path(report)
    evidence = report.with_name(report.name + ".phases")
    safe_path(evidence)
    if report == runtime or runtime in report.parents:
        raise Blocked("Report must be outside companion data")
    if report.exists() or evidence.exists():
        raise Blocked("Report or phase evidence already exists")
    if runtime.exists() and (not runtime.is_dir() or any(runtime.iterdir())):
        raise Blocked("Runtime root must be empty")
    private = runtime / "RuntimeData/Companions/qualification/v1"
    critical = (private / "RuntimeData/Identity/identity.json.tmp", private / "Memory/revia_memory.db-journal")
    if os.name == "nt" and any(len(str(path)) >= 250 for path in critical):
        raise Blocked("Captured runtime path leaves insufficient Windows atomic-file path budget")
    return runtime, report, evidence


def string_fields(value, prefix="body"):
    if isinstance(value, str):
        yield prefix, value
    elif isinstance(value, dict):
        for key, child in value.items():
            yield from string_fields(child, prefix + "." + key)
    elif isinstance(value, list):
        for index, child in enumerate(value):
            yield from string_fields(child, f"{prefix}[{index}]")


def admitted(body):
    records = []
    for message in body.get("messages", []):
        content = message.get("content")
        if isinstance(content, str) and MEMORY_MARKER in content:
            for line in content.split(MEMORY_MARKER, 1)[1].splitlines():
                if line.startswith("- [project] [subject: participant ") and "Project Juniper's label" in line and "[historical owner-revised record;" not in line:
                    records.append(line)
    return records


def routing_checks(requests):
    requests = [request for request in requests if request["stage"] == "recall/public"]
    checks = {"public_request_observed": "passed" if requests else "failed"}
    for i, request in enumerate(requests):
        for field, text in string_fields(request["body"]):
            for sentinel in SENTINELS:
                checks[f"public[{i}].{field}.excludes_{sentinel}"] = "failed" if sentinel in text else "passed"
        checks[f"public[{i}].classification_excluded"] = "failed" if "response_format" in request["body"] else "passed"
    return checks


class SyntheticProvider:
    def __init__(self, port, secret):
        self.secret, self.stage, self.requests = secret, "startup", []
        self.metadata_requests, self.errors, self.unauthorized = [], [], 0
        fixture = self

        class Protocol(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def send(self, value, code=200, media="application/json"):
                raw = (value if isinstance(value, str) else json.dumps(value)).encode()
                self.send_response(code)
                self.send_header("Content-Type", media)
                self.send_header("Content-Length", str(len(raw)))
                self.end_headers()
                self.wfile.write(raw)

            def do_GET(self):
                fixture.metadata_requests.append(self.path)
                values = {"/health": {"status": "ok", "slots_idle": 1}, "/v1/models": {"data": [{"id": MODEL}]},
                          "/props": {"total_slots": 1, "default_generation_settings": {"n_ctx": 8192}}}
                self.send(values.get(self.path, {"error": "unknown endpoint"}), 200 if self.path in values else 404)

            def do_POST(self):
                if self.headers.get("Authorization") != "Bearer " + fixture.secret:
                    fixture.unauthorized += 1
                    self.send({"error": "authentication required"}, 401)
                    return
                try:
                    size = int(self.headers.get("Content-Length", "0"))
                    if not 0 < size < 1048576:
                        raise ValueError("request size")
                    body = unique_json(self.rfile.read(size).decode())
                    if self.path == "/fixture/stage":
                        fixture.stage = body["stage"]
                        self.send({"id": MODEL})
                        return
                    if self.path != "/v1/chat/completions":
                        self.send({"error": "unknown endpoint"}, 404)
                        return
                    answer = fixture.answer(body)
                    fixture.requests.append({"stage": fixture.stage, "body": redact(body, secret), "authenticated": True,
                                             "observed_provider": MODEL, "synthetic_reply": answer})
                    choice = {"message": {"role": "assistant", "content": answer}, "finish_reason": "stop"}
                    if body.get("stream"):
                        chunk = {"choices": [{"delta": {"content": answer}, "finish_reason": "stop"}]}
                        self.send("data: " + json.dumps(chunk) + "\n\ndata: [DONE]\n\n", media="text/event-stream")
                    else:
                        self.send({"model": MODEL, "choices": [choice]})
                except (ValueError, KeyError, TypeError):
                    fixture.errors.append("malformed authenticated request")
                    self.send({"error": "malformed request"}, 400)

        class Exclusive(http.server.HTTPServer):
            allow_reuse_address = False

            def __init__(self, *args):
                self.accepted_lock = threading.Lock()
                self.accepted = set()
                self.closing = False
                self.aborted_on_close = 0
                super().__init__(*args)

            def get_request(self):
                connection, address = super().get_request()
                connection.settimeout(.5)
                with self.accepted_lock:
                    if self.closing:
                        connection.close()
                        raise OSError("Fixture provider is closing")
                    self.accepted.add(connection)
                return connection, address

            def shutdown_request(self, connection):
                try:
                    super().shutdown_request(connection)
                finally:
                    with self.accepted_lock:
                        self.accepted.discard(connection)

            def close_accepted(self):
                with self.accepted_lock:
                    self.closing = True
                    active = list(self.accepted)
                    self.aborted_on_close = len(active)
                for connection in active:
                    try:
                        connection.shutdown(socket.SHUT_RDWR)
                    except OSError:
                        pass
                    connection.close()

            def handle_error(self, *args):
                if not self.closing:
                    fixture.errors.append("Fixture request handler failed")

            def server_bind(self):
                if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
                    self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
                super().server_bind()

        self.server = Exclusive(("127.0.0.1", port), Protocol)
        self.thread = threading.Thread(target=lambda: self.server.serve_forever(poll_interval=.05), daemon=True, name="qual1-fixture")
        self.shutdown_thread = None
        self.cleanup = "not_started"

    def answer(self, body):
        if body.get("response_format", {}).get("type") == "json_object":
            try:
                candidate = unique_json(body["messages"][-1]["content"])
            except (ValueError, KeyError, TypeError):
                candidate = {}
            text = candidate.get("user_message")
            if candidate.get("captured_speaker_known") is True and text in (INITIAL, CORRECTION):
                label = "Cedar" if text == INITIAL else "Maple"
                return json.dumps({"shouldRemember": True, "category": "project", "subject": "speaker",
                    "summary": f"The user uses {label} as Project Juniper's label.", "reason": "Synthetic attributed project extraction."})
            return json.dumps({"shouldRemember": False, "reason": "No fixture statement admitted."})
        if "response_format" in body:
            return json.dumps({"verdict": "allow", "reason": "Synthetic provider."})
        evidence = admitted(body)
        if any("Maple" in record and "[current owner revision; receipt:" in record for record in evidence) and not any("Cedar" in record for record in evidence):
            return "Project Juniper's current label is Maple."
        return "No admitted current label is available."

    def __enter__(self):
        self.thread.start()
        self.cleanup = "pending"
        return self

    def __exit__(self, *args):
        self.cleanup = "uncertain"
        self.server.close_accepted()
        self.shutdown_thread = threading.Thread(target=self.server.shutdown, daemon=True, name="qual1-fixture-stop")
        self.shutdown_thread.start()
        self.shutdown_thread.join(1.25)
        self.server.server_close()
        self.thread.join(1.25)
        with self.server.accepted_lock:
            active = bool(self.server.accepted)
        if self.shutdown_thread.is_alive() or self.thread.is_alive() or active:
            raise RuntimeError("Fixture provider cleanup uncertain")
        self.cleanup = "closed"


def run_child(command, receipt, phase, run_id, runtime, fault, env, secret, timeout):
    began = time.monotonic()
    child = subprocess.Popen(command, cwd=runtime, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    result = {"phase": phase, "pid": child.pid, "status": "failed", "checks": {}, "cleanup": "pending",
              "credential_in_phase_receipt": False, "phase_receipt_redaction": "not_needed"}
    try:
        stdout, stderr = child.communicate(timeout=timeout)
        result["cleanup"] = "exited"
    except subprocess.TimeoutExpired:
        result["status"] = "blocked"
        child.kill()
        try:
            stdout, stderr = child.communicate(timeout=3)
            result["cleanup"] = "terminated_owned_process"
        except subprocess.TimeoutExpired:
            stdout, stderr = b"", b""
            result["cleanup"] = "uncertain"
    result.update(process_exit=child.poll(), seconds=round(time.monotonic() - began, 4),
                  stdout=redact(stdout.decode(errors="replace"), secret), stderr=redact(stderr.decode(errors="replace"), secret),
                  credential_in_child_output=bool(secret and (secret.encode() in stdout or secret.encode() in stderr)))
    try:
        raw_receipt = receipt.read_bytes()
        if secret and secret.encode() in raw_receipt:
            result["credential_in_phase_receipt"] = True
            result["phase_receipt_redaction"] = "failed"
            receipt.write_bytes(raw_receipt.replace(secret.encode(), b"[REDACTED]"))
            result["phase_receipt_redaction"] = "completed"
        receipt_text = raw_receipt.decode("utf-8")
        value = unique_json(receipt_text)
        required = {"phase": phase, "run_id": run_id, "pid": child.pid, "runtime_root": str(runtime), "fault": fault,
                    "schema_version": 1, "scope": "fixture", "real_model": "not_run"}
        if not isinstance(value, dict) or any(value.get(k) != v for k, v in required.items()):
            raise ValueError("phase identity mismatch")
        if not set(value).issubset(CHILD_RECEIPT_FIELDS):
            raise ValueError("unknown or runner-owned receipt field")
        if value.get("status") not in STATES or not isinstance(value.get("checks"), dict) or any(v not in STATES for v in value["checks"].values()):
            raise ValueError("phase outcomes malformed")
        if result["status"] != "blocked":
            result.update({k: v for k, v in redact(value, secret).items() if k not in {"pid", "phase"}})
        if result["status"] == "passed" and child.returncode != 0:
            result["status"] = "failed"
    except (OSError, ValueError, TypeError) as error:
        result["error"] = "Invalid/missing phase receipt: " + type(error).__name__
        if result["status"] != "blocked":
            result["status"] = "failed"
    if result["credential_in_child_output"] or result["credential_in_phase_receipt"]:
        detail = "receipt sanitization failed" if result["phase_receipt_redaction"] == "failed" else "sanitized evidence retained"
        result["status"], result["error"] = "failed", "Credential observed; " + detail
    return result


def positive_oracle(write, recall):
    revision = write.get("durable_receipt") or {}
    before, after = write.get("relationship", {}), recall.get("relationship_loaded", {})
    introduced = recall.get("relationship", {})
    mandatory = {"no_child_processes_allowed", "optional_hosts_disabled_before_start", "memory_enabled_no_archive_tail", "production_stop"}
    write_checks = write.get("checks", {})
    recall_checks = recall.get("checks", {})
    request_id, revised_id = revision.get("request_id", ""), revision.get("revised_id", "")
    lines = recall.get("retrieved_active", [])
    linked = bool(request_id and revised_id and lines) and all(
        f"[source: automatic; id: {revised_id}]" in line and
        f"[current owner revision; receipt: {request_id}; current id: {revised_id}]" in line and
        f"[subject: participant {revision.get('subject_id', '')}]" in line for line in lines)
    return bool(write.get("status") == recall.get("status") == "passed"
        and write.get("process_exit") == recall.get("process_exit") == 0 and write.get("pid") != recall.get("pid")
        and revision.get("request_id") and revision.get("digest") and revision.get("original_id") != revision.get("revised_id")
        and write.get("revision_count") == recall.get("revision_count") == 1
        and write.get("companion_id") == recall.get("companion_id") == "qualification"
        and before.get("entity_id") == after.get("entity_id") == revision.get("subject_id")
        and introduced.get("entity_id") == before.get("entity_id")
        and before.get("display_name") == after.get("display_name") == introduced.get("display_name") == "QualificationOwner"
        and before.get("first_seen") == after.get("first_seen")
        and after.get("interactions", 0) >= before.get("interactions", 0) > 0
        and linked and all("Maple" in text and "Cedar" not in text for text in lines)
        and recall.get("working_context_before_introduction") == 0
        and "Maple" in recall.get("answer", "") and "Cedar" not in recall.get("answer", "")
        and "Maple" in recall.get("private_control_answer", "") and recall.get("routing_passed") is True
        and mandatory.union({"initial_automatic_save", "automatic_proposal", "durable_acceptance"}).issubset(write_checks)
        and mandatory.union({"public_private_rows_unchanged", "public_private_revisions_unchanged"}).issubset(recall_checks)
        and all(v == "passed" for v in write_checks.values()) and all(v == "passed" for v in recall_checks.values()))


def make_configuration(root, port, run_id):
    private = root / "RuntimeData/Companions/qualification/v1"
    settings = {name: {"enabled": False} for name in ("embedding", "speechRecognition", "vision", "perception", "bargeIn", "image", "video", "performance", "memoryReranker", "memoryNli", "improvement", "localApi")}
    settings["embedding"]["autoStartServer"] = False
    settings.update(activeProfile="fixture", llm={"backend": "LLamaCpp", "host": "127.0.0.1", "port": port,
        "modelName": MODEL, "autoStartServer": False, "visionEnabled": False, "autoTune": False,
        "serverExecutable": str(root / "missing.exe"), "modelPath": str(root / "missing.gguf"),
        "contextSize": 8192, "maxTokens": 256, "cloud": {"enabled": False}},
        intelligence={"enabled": False, "fast": {"enabled": False, "warmAtStartup": False}, "expert": {"enabled": False, "warmAtStartup": False}},
        speech={"enabled": False, "speakGreeting": False, "backend": "WindowsSapi", "voiceDataPath": str(private / "RuntimeData/Voices")},
        presence={"enabled": False, "avatarBridgeEnabled": False, "externalAdaptersEnabled": False},
        initiative={"enabled": False, "curiosityEnabled": False, "spontaneousSpeechEnabled": False},
        conversation={"archiveEnabled": False, "restoreTurns": 0, "selfInquiryEnabled": False, "delegation": "off"},
        responseFilter={"aiReviewEnabled": False}, mcp={"servers": []})
    profile = {"id": "fixture", "displayName": "Fixture", "systemPrompt": "You are a synthetic companion fixture.", "shouldSpeak": False, "memoryEnabled": True}
    root.mkdir(parents=True, exist_ok=True)
    for path, value in ((root / "Config/settings.json", settings), (private / "Config/Profiles/fixture.json", profile),
                        (root / "qualification-owner.json", {"run_id": run_id, "scope": "fixture"})):
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("x", encoding="utf-8") as output:
            json.dump(value, output, indent=2)
    return {"settings": settings, "profile": profile, "sha256": identity({"settings": settings, "profile": profile})}


def run(args):
    secret = os.environ.get(args.api_key_env, "")
    report = {"schema_version": 1, "scope": "fixture", "real_model": "not_run", "status": "blocked", "phases": [], "checks": {},
              "run_id": uuid.uuid4().hex, "fault": args.fault, "provider_cleanup": "not_started", "report_written": False,
              "requested_provider": MODEL, "observed_provider": None, "positive_restart_oracle": False}
    destination = None
    try:
        root, target, evidence = fresh_paths(Path(args.runtime_root), Path(args.report))
        target.parent.mkdir(parents=True, exist_ok=True)
        with target.open("x", encoding="utf-8") as output:
            json.dump(report, output)
        destination = target
        report["report_written"] = True
        if not args.fixture or args.scenario != "memory-restart":
            raise Blocked("Only explicit fixture memory-restart is implemented; live mode not_run")
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", args.api_key_env) or len(secret) < 16:
            raise Blocked("Missing named fixture credential (minimum 16 characters)")
        if args.timeout <= 0 or not 0 <= args.provider_port <= 65535:
            raise Blocked("Invalid finite timeout or provider port")
        worker = safe_path(Path(args.worker))
        if not worker.is_file():
            raise Blocked("Native worker missing")
        report["source"] = source_identity()
        report["worker"] = {"path": str(worker), "sha256": sha(worker.read_bytes())}
        report["workload"] = {"initial": INITIAL, "correction": CORRECTION, "question": "What is Project Juniper's current label?", "injected_tail": False}
        report["workload_sha256"] = identity(report["workload"])
        evidence.mkdir()
        try:
            provider = SyntheticProvider(args.provider_port, secret)
        except OSError as error:
            raise Blocked("Owned provider port unavailable; " + type(error).__name__) from error
        with provider:
            report["provider_cleanup"] = "pending"
            report["configuration"] = make_configuration(root, provider.server.server_port, report["run_id"])
            environment = {k: v for k, v in os.environ.items() if not k.startswith("REVIA_")}
            environment[args.api_key_env] = secret
            environment["PYTHONDONTWRITEBYTECODE"] = "1"
            for phase in ("write-correct", "recall"):
                path = evidence / (phase + ".json")
                command = [str(worker), "--phase", phase, "--runtime-root", str(root), "--provider-port", str(provider.server.server_port),
                    "--api-key-env", args.api_key_env, "--receipt", str(path), "--fault", args.fault, "--run-id", report["run_id"]]
                entry = run_child(command, path, phase, report["run_id"], root, args.fault, environment, secret, args.timeout)
                entry["command"] = command
                report["phases"].append(entry)
                if sha(worker.read_bytes()) != report["worker"]["sha256"]:
                    entry["status"], entry["error"] = "failed", "Worker binary changed"
                if entry["status"] != "passed" or entry["process_exit"] != 0:
                    break
            report["requests"] = provider.requests
            report["observed_provider"] = MODEL if "/v1/models" in provider.metadata_requests else None
            report["metadata_requests"], report["provider_errors"] = provider.metadata_requests, provider.errors
            report["unauthorized_startup_probes"] = provider.unauthorized
        report["provider_cleanup"] = "closed"
        if len(report["phases"]) == 2:
            first, second = report["phases"]
            second["retrieved_active"] = [line for request in report["requests"] if request["stage"] == "recall/private" and "response_format" not in request["body"] for line in admitted(request["body"])]
            report["checks"].update(routing_checks(report["requests"]))
            second["routing_passed"] = all(state == "passed" for state in report["checks"].values())
            report["positive_restart_oracle"] = positive_oracle(first, second)
        report["checks"]["positive_restart_oracle"] = "passed" if report["positive_restart_oracle"] else "failed"
        leaks = [str(path) for directory in (root, evidence) for path in directory.rglob("*") if path.is_file() and secret.encode() in path.read_bytes()]
        report["checks"]["credential_absent_from_artifacts"] = "failed" if leaks else "passed"
        report["credential_leak_paths"] = leaks
        report["checks"]["source_unchanged"] = "passed" if source_identity() == report["source"] else "failed"
        report["status"] = "passed" if report["positive_restart_oracle"] and not leaks and not report["provider_errors"] and all(v == "passed" for v in report["checks"].values()) else "failed"
    except Blocked as error:
        report["error"], report["status"] = str(error), "blocked"
    except Exception as error:
        report["error"], report["status"] = type(error).__name__ + ": " + str(error), "failed"
        if report["provider_cleanup"] == "pending":
            report["provider_cleanup"] = "uncertain"
    present = {phase["phase"] for phase in report["phases"]}
    for phase in ("write-correct", "recall"):
        if phase not in present:
            report["phases"].append({"phase": phase, "status": "not_run", "process_exit": None, "checks": {}})
    counted = [entry["status"] for entry in report["phases"]] + [state for entry in report["phases"] for state in entry["checks"].values()] + list(report["checks"].values()) + ["not_run", "not_run"]
    report["counts"], report["denominator"] = {state: Counter(counted)[state] for state in STATES}, len(counted)
    report["unexecuted"] = ["real_model", "full_suite", "Q02 lifecycle/revocation and later qualification scenarios"]
    if destination:
        destination.write_text(json.dumps(redact(report, secret), indent=2) + "\n", encoding="utf-8")
    return report


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ("scenario", "runtime-root", "api-key-env", "report", "worker"):
        p.add_argument("--" + name, required=True)
    p.add_argument("--fixture", action="store_true")
    p.add_argument("--provider-port", type=int, required=True)
    p.add_argument("--fault", choices=FAULTS, default="none")
    p.add_argument("--timeout", type=float, default=45)
    return p


if __name__ == "__main__":
    result = run(parser().parse_args())
    print(json.dumps({key: result.get(key) for key in ("status", "scope", "real_model", "report_written", "counts", "error")}))
    raise SystemExit(0 if result["status"] == "passed" else 1)
