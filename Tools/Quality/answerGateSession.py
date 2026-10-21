"""Run a finite answer-gate campaign on one owned, authenticated loopback model.

This starts an evaluation-only process when explicitly invoked. It never attaches to
an app session, reads app credentials, changes profiles/settings, or commits results.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import threading
import time
import uuid

import contextBenchmark as backend

KEY_ENV = "REVIA_OWNED_EVALUATION_KEY"
EXPERIMENTS = ("control", "omit-system-prompt", "omit-state-packet", "omit-style-guidance")


def validate_plan(args):
    if not 1 <= len(args.seeds) <= 4 or len(set(args.seeds)) != len(args.seeds) or any(seed < 0 or seed > 2**64 - 1 for seed in args.seeds):
        raise ValueError("Declare one to four distinct unsigned 64-bit seeds.")
    if not 180 <= args.timeout_seconds <= 28800:
        raise ValueError("The entire session timeout must be between 180 and 28800 seconds.")
    if not 1 <= args.max_tokens < args.context:
        raise ValueError("Output tokens must be positive and smaller than the declared context.")
    if args.record_baseline and (args.experiment != "control" or args.category or len(args.seeds) < 2 or args.no_judge):
        raise ValueError("Baseline recording requires a full-corpus control with at least two declared seeds.")


def gate_arguments(args, port, alias, seed, directory):
    command = [str(args.gate), str(args.profile), str(args.corpus), str(port),
               "--output-dir", str(directory), "--expected-model", alias, "--api-key-env", KEY_ENV,
               "--seed", str(seed), "--context-size", str(args.context), "--max-tokens", str(args.max_tokens),
               "--max-unavailable", "0", "--min-judge-coverage", "100", "--commit", args.commit]
    if args.baseline:
        command += ["--baseline", str(args.baseline)]
    else:
        command += ["--ignore-baseline"]
    if args.experiment != "control":
        command.append("--" + args.experiment)
    if args.no_judge:
        command.append("--no-judge")
    for category in args.category:
        command += ["--category", category]
    if args.record_baseline and seed == args.seeds[0]:
        command += ["--write-baseline", str(args.output / "baseline-candidate.json")]
    return command


def copy_redacted_stream(source, target, secret):
    pending = b""
    read = getattr(source, "read1", source.read)
    while chunk := read(65536):
        pending = (pending + chunk).replace(secret, b"[REDACTED]")
        safe = max(0, len(pending) - len(secret) + 1)
        target.write(pending[:safe])
        target.flush()
        pending = pending[safe:]
    target.write(pending.replace(secret, b"[REDACTED]"))
    target.flush()


class OwnedChild:
    def __init__(self, arguments, directory, environment, log_path, key):
        self.arguments, self.directory, self.environment = arguments, directory, environment
        self.log_path, self.key = log_path, key
        self.process = self.log = self.reader = None
        self.log_error = None

    def __enter__(self):
        self.log = self.log_path.open("xb")
        try:
            self.process = subprocess.Popen(self.arguments, cwd=self.directory, env=self.environment,
                                            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                            stderr=subprocess.STDOUT, creationflags=backend.hidden_flags())
            def copy():
                try:
                    copy_redacted_stream(self.process.stdout, self.log, self.key.encode("ascii"))
                except BaseException as error:
                    self.log_error = error
            self.reader = threading.Thread(target=copy, daemon=True)
            self.reader.start()
            return self
        except BaseException:
            if self.process is not None:
                backend.stop_owned_process(self.process)
            self.log.close()
            raise

    def __exit__(self, *_):
        try:
            backend.stop_owned_process(self.process)
        finally:
            if self.reader:
                self.reader.join(timeout=15)
                if self.reader.is_alive():
                    raise RuntimeError("Owned process log did not finish; evidence is incomplete.")
            if self.process.stdout:
                self.process.stdout.close()
            self.log.close()
        if self.log_error:
            raise RuntimeError("Owned process log could not be retained completely.")


def argument_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("server", "model", "gate", "profile", "corpus", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--device", choices=("CUDA0", "CUDA1"), default="CUDA0")
    parser.add_argument("--context", type=int, choices=(8192, 16384, 32768), default=8192)
    parser.add_argument("--max-tokens", type=int, default=512)
    parser.add_argument("--cache-type-k", choices=("f16", "q8_0"), default="f16")
    parser.add_argument("--cache-type-v", choices=("f16", "q8_0"), default="f16")
    parser.add_argument("--seeds", type=int, nargs="+", default=[0, 1])
    parser.add_argument("--timeout-seconds", type=int, default=10800)
    parser.add_argument("--experiment", choices=EXPERIMENTS, default="control")
    parser.add_argument("--category", action="append", default=[])
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--record-baseline", action="store_true",
                        help="Retain a candidate baseline from the first seed; never commits or overwrites it.")
    parser.add_argument("--no-judge", action="store_true", help="Deterministic-only experiment; cannot record a baseline.")
    parser.add_argument("--commit", default="")
    return parser


def run_session(args):
    validate_plan(args)
    for field in ("server", "model", "gate", "profile", "corpus"):
        path = getattr(args, field).resolve(strict=True)
        if not path.is_file():
            raise ValueError("Every selected executable, model, profile, and corpus must be an existing file.")
        setattr(args, field, path)
    if args.baseline:
        args.baseline = args.baseline.resolve(strict=True)
    args.output = args.output.absolute()
    args.output.mkdir(parents=True, exist_ok=False)
    # --help is bounded and loads no model; refusal happens before a listener exists.
    backend.require_api_key_file_support(args.server)
    inputs = {name: {"path": str(getattr(args, name)), "sha256": backend.sha256_file(getattr(args, name))}
              for name in ("server", "model", "gate", "profile", "corpus")}
    inputs["harness"] = {"path": str(Path(__file__)), "sha256": backend.sha256_file(Path(__file__))}
    inputs["backendHelpers"] = {"path": str(Path(backend.__file__)), "sha256": backend.sha256_file(Path(backend.__file__))}
    report = {"schemaVersion": 1, "evidenceKind": "owned_local_answer_gate",
              "inputs": inputs, "startedAt": backend.utc_now(), "seeds": args.seeds,
              "experiment": args.experiment, "categories": args.category, "context": args.context,
              "maxTokens": args.max_tokens, "cacheTypeK": args.cache_type_k, "cacheTypeV": args.cache_type_v,
              "sameModelJudge": not args.no_judge, "judgeEnabled": not args.no_judge,
              "judgeLimitation": "No judge." if args.no_judge else "The candidate model also judges; no independent model-quality oracle.",
              "memoryEnabled": False, "settingsChanged": False, "committed": False,
              "timeoutSeconds": args.timeout_seconds, "runs": [], "status": "starting"}
    backend.write_new_json(args.output / "plan.json", report)
    environment = {name: value for name, value in os.environ.items()
                   if not name.startswith("LLAMA_ARG_") and name != KEY_ENV}
    environment.update(HF_HUB_OFFLINE="1", TRANSFORMERS_OFFLINE="1")
    port, alias = backend.fresh_port(), "revia-answer-" + uuid.uuid4().hex
    deadline = time.monotonic() + args.timeout_seconds
    server_process = None
    secret = ""
    try:
        with backend.EphemeralApiKey() as credential:
            secret = credential.value
            arguments = backend.server_arguments(args.server, args.model, args.context, port, alias, args.device,
                                                 args.cache_type_k, args.cache_type_v, api_key_file=credential.path)
            report.update(port=port, alias=alias, serverArguments=arguments)
            backend.write_new_json(args.output / "launch.json", {"arguments": arguments, "credential": "owned ephemeral file; value omitted"})
            with OwnedChild(arguments, args.server.parent, environment, args.output / "server.log", secret) as server:
                server_process = server.process
                report["ownedServerPid"] = server_process.pid
                properties = backend.wait_ready(server_process, port, alias, args.context, api_key=secret)
                backend.write_new_json(args.output / "properties.json", properties)
                child_environment = dict(environment, **{KEY_ENV: secret})
                for seed in args.seeds:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise TimeoutError("The finite evaluation session deadline elapsed.")
                    directory = args.output / ("seed-" + str(seed))
                    arguments = gate_arguments(args, port, alias, seed, directory)
                    started = time.perf_counter()
                    print(f"Answer gate seed {seed}: {args.experiment}, context {args.context}.", flush=True)
                    with OwnedChild(arguments, args.output, child_environment, args.output / f"seed-{seed}.log", secret) as gate:
                        code = gate.process.wait(timeout=remaining)
                        report["runs"].append({"seed": seed, "arguments": arguments, "ownedGatePid": gate.process.pid,
                                               "exitCode": code, "elapsedSeconds": time.perf_counter() - started})
                    if code not in (0, 1):
                        raise RuntimeError(f"Gate seed {seed} was incomplete (exit {code}).")
                    summaries = list(directory.glob("*.summary.json"))
                    if len(summaries) != 1:
                        raise RuntimeError("An owned gate run did not retain exactly one summary.")
                    summary = json.loads(summaries[0].read_text(encoding="utf-8"))
                    coverage = summary["summary"]["overall"]
                    report["runs"][-1].update(summary=str(summaries[0]), summarySha256=backend.sha256_file(summaries[0]),
                                              gateOutcome=summary["gate"]["outcome"],
                                              coverage={name: coverage[name] for name in ("cases", "judged", "unavailable")},
                                              judgeEnabled=not args.no_judge)
                    if (coverage["cases"] <= 0 or coverage["unavailable"] != 0 or
                            (not args.no_judge and coverage["judged"] != coverage["cases"])):
                        raise RuntimeError("The repeated run did not answer and judge every selected case.")
                report["status"] = "completed"
    except BaseException as error:
        report["status"] = "interrupted" if isinstance(error, KeyboardInterrupt) else "incomplete"
        report["error"] = str(error).replace(secret, "[REDACTED]") if secret else str(error)
    finally:
        report["ownedServerStopped"] = server_process is None or server_process.poll() is not None
        report["finishedAt"] = backend.utc_now()
        try:
            report["inputsUnchanged"] = all(backend.sha256_file(Path(item["path"])) == item["sha256"] for item in inputs.values())
        except OSError:
            report["inputsUnchanged"] = False
        if not report["inputsUnchanged"] or not report["ownedServerStopped"]:
            report["status"] = "incomplete"
        backend.write_new_json(args.output / "session.json", report)
    print(f"Evidence: {args.output}; {report['status']}.", flush=True)
    if report["status"] != "completed":
        return 2
    return 1 if any(run["exitCode"] == 1 for run in report["runs"]) else 0


def main():
    parser = argument_parser()
    args = parser.parse_args()
    try:
        return run_session(args)
    except (OSError, RuntimeError, ValueError, subprocess.SubprocessError) as error:
        print(f"Evaluation session refused: {error}", flush=True)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
