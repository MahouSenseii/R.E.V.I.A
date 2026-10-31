"""Turn a verified base checkpoint and a training candidate into llama.cpp files, then check them.

Optional and offline. `base` converts a verified Hugging Face base to a BF16 GGUF and quantizes
it: the matched baseline. `adapter` converts a candidate adapter to a GGUF LoRA for exactly
that base, so the serving base never changes and dropping --lora rolls it back. `check` starts
llama-server on the result and compares its embedded template, chat rendering (including tool
definitions), tokenization and end-of-turn token with the Hugging Face base. Each command
writes a new directory with a receipt; nothing is activated.

The converter scripts run under --converter-python, an environment built from the pinned
llama.cpp tag's own requirements, separate from the training environment.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request


def _load_worker():
    spec = importlib.util.spec_from_file_location("revia_training_worker", Path(__file__).with_name("revia_training_worker.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


WORKER = _load_worker()
require, digest, plain = WORKER.require, WORKER.digest, WORKER.plain
SAMPLES = [
    [{"role": "system", "content": "You are Revia."}, {"role": "user", "content": "How did the poem make you feel?"}],
    [{"role": "system", "content": "You are Revia."}, {"role": "user", "content": "Write add(a, b) in Python."},
     {"role": "assistant", "content": "def add(a, b):\n    return a + b"}, {"role": "user", "content": "Now review it."}],
]
TOOLS = [{"type": "function", "function": {"name": "read_file", "description": "Read one file.",
                                           "parameters": {"type": "object", "properties": {"path": {"type": "string"}},
                                                          "required": ["path"]}}}]


def read_gguf_header(path):
    """Metadata keys and tensor count from a GGUF header, without loading tensors."""
    sizes = {0: "B", 1: "b", 2: "H", 3: "h", 4: "I", 5: "i", 6: "f", 7: "?", 10: "Q", 11: "q", 12: "d"}
    with Path(path).open("rb") as stream:
        def take(fmt):
            size = struct.calcsize("<" + fmt)
            data = stream.read(size)
            require(len(data) == size, f"{Path(path).name} ends inside its GGUF header.")
            return struct.unpack("<" + fmt, data)[0]

        def text():
            length = take("Q")
            require(length <= 64 * 1024 * 1024, "A GGUF string exceeds its bound.")
            return stream.read(length).decode("utf-8", "replace")

        def value(kind):
            if kind in sizes:
                return take(sizes[kind])
            if kind == 8:
                return text()
            require(kind == 9, f"Unsupported GGUF value type {kind}.")
            inner, count = take("I"), take("Q")
            if inner == 8:
                for _ in range(count):
                    text()
            else:
                stream.seek(struct.calcsize("<" + sizes[inner]) * count, os.SEEK_CUR)
            return f"<array of {count}>"

        require(stream.read(4) == b"GGUF", f"{Path(path).name} is not a GGUF file.")
        version, tensors, keys = take("I"), take("Q"), take("Q")
        metadata = {}
        for _ in range(keys):
            key = text()
            metadata[key] = value(take("I"))
    return {"version": version, "tensors": tensors, "metadata": metadata}


def gguf_identity(path):
    header = read_gguf_header(path)
    metadata = header["metadata"]
    architecture = metadata.get("general.architecture", "")
    template = metadata.get("tokenizer.chat_template")
    return {"sha256": digest(path), "bytes": Path(path).stat().st_size, "architecture": architecture,
            "type": metadata.get("general.type", "model"), "blockCount": metadata.get(f"{architecture}.block_count"),
            # A kept MTP head is stored as extra blocks after the decoder layers.
            "nextnLayers": metadata.get(f"{architecture}.nextn_predict_layers", 0),
            "fileType": metadata.get("general.file_type"), "tensors": header["tensors"],
            "chatTemplateSha256": hashlib.sha256(template.encode()).hexdigest() if isinstance(template, str) else None}


def run(label, command, timeout):
    completed = subprocess.run([str(part) for part in command], stdin=subprocess.DEVNULL, capture_output=True, text=True,
                               timeout=timeout, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    require(completed.returncode == 0, f"{label} failed with exit {completed.returncode}: {completed.stderr[-2000:]}")
    return completed


def new_directory(path):
    path = plain(Path(path).absolute())
    staging = path.parent / f".{path.name}.partial"
    require(not path.exists() and not staging.exists(), f"{path} or its unfinished staging directory already exists; it is never replaced.")
    staging.mkdir(parents=True)
    return path, staging


def finish(staging, final, record):
    files = {item.name: {"sha256": digest(item), "bytes": item.stat().st_size} for item in sorted(staging.iterdir()) if item.is_file()}
    record["files"] = files
    (staging / "export.json").write_text(json.dumps(record, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.rename(staging, final)
    return record


def verify_base(arguments):
    return WORKER.verify_base({"base": {"directory": arguments.base, "repository": arguments.repository, "revision": arguments.revision,
                                        "installManifestSha256": arguments.install_manifest_sha256}})


def converter_identity(llama_cpp):
    llama_cpp = plain(Path(llama_cpp))
    identity = {"directory": str(llama_cpp), "convertHfSha256": digest(llama_cpp / "convert_hf_to_gguf.py"),
                "convertLoraSha256": digest(llama_cpp / "convert_lora_to_gguf.py")}
    head = subprocess.run(["git", "-C", str(llama_cpp), "rev-parse", "HEAD"], capture_output=True, text=True)
    identity["commit"] = head.stdout.strip() if head.returncode == 0 else ""
    return identity


def verify_candidate(directory, base):
    directory = plain(Path(directory))
    record = json.loads(WORKER.read_bounded(directory / "candidate.json", 1 << 20))
    require(record.get("schema") == WORKER.CANDIDATE_SCHEMA and record.get("state") == "candidate", "This is not a training candidate.")
    # An adapter only means something on the exact base it was trained on: a 4B adapter is never a 9B one.
    require(record.get("base") == base, "The candidate was trained on a different base checkpoint; it is not converted for this one.")
    for name, entry in record.get("files", {}).items():
        require(digest(directory / name) == entry["sha256"], f"Candidate file {name} changed after the job published it.")
    return record


def checkpoint_tensor_names(directory):
    index = directory / "model.safetensors.index.json"
    if index.exists():
        return set(json.loads(WORKER.read_bounded(index, 64 << 20))["weight_map"])
    names = set()
    for path in sorted(directory.glob("*.safetensors")):
        with path.open("rb") as stream:
            length = struct.unpack("<Q", stream.read(8))[0]
            require(length <= 64 << 20, f"{path.name} has an unbounded safetensors header.")
            names |= set(json.loads(stream.read(length))) - {"__metadata__"}
    return names


def mtp_head(directory):
    """(declared, present): a config can declare an MTP head its weights do not carry."""
    config = json.loads(WORKER.read_bounded(directory / "config.json", 1 << 20))
    declared = int(config.get("text_config", config).get("mtp_num_hidden_layers", 0) or 0) > 0
    return declared, any(name.startswith(("mtp.", "model.mtp.")) for name in checkpoint_tensor_names(directory))


def command_base(arguments):
    base = verify_base(arguments)
    final, staging = new_directory(arguments.output)
    try:
        llama = Path(arguments.llama_cpp)
        bf16 = staging / "model-bf16.gguf"
        declared, present = mtp_head(Path(arguments.base))
        # Converting a declared head without its weights writes a block the loader cannot find.
        omit = ["--no-mtp"] if declared and not present else []
        run("Base conversion", [arguments.converter_python, llama / "convert_hf_to_gguf.py", arguments.base, "--outtype", "bf16",
                                "--outfile", bf16, *omit], arguments.timeout)
        quantized = staging / f"model-{arguments.quantization}.gguf"
        run("Quantization", [arguments.quantize, bf16, quantized, arguments.quantization], arguments.timeout)
        record = {"kind": "base", "base": base, "converter": converter_identity(llama), "quantization": arguments.quantization,
                  "mtpHead": declared and present,
                  "gguf": {bf16.name: gguf_identity(bf16), quantized.name: gguf_identity(quantized)}}
        return finish(staging, final, record)
    except BaseException:
        shutil.rmtree(staging, ignore_errors=True)
        raise


def command_adapter(arguments):
    base = verify_base(arguments)
    candidate = verify_candidate(arguments.candidate, base)
    final, staging = new_directory(arguments.output)
    try:
        llama = Path(arguments.llama_cpp)
        adapter = staging / "adapter-f16.gguf"
        run("Adapter conversion", [arguments.converter_python, llama / "convert_lora_to_gguf.py", "--base", arguments.base, "--outtype", "f16",
             "--outfile", adapter, arguments.candidate], arguments.timeout)
        record = {"kind": "adapter", "base": base, "converter": converter_identity(llama),
                  "candidate": {"jobId": candidate["jobId"], "candidateManifestSha256": digest(Path(arguments.candidate) / "candidate.json"),
                                "configurationSha256": candidate["configurationSha256"], "dataset": candidate["dataset"]},
                  "gguf": {adapter.name: gguf_identity(adapter)}}
        return finish(staging, final, record)
    except BaseException:
        shutil.rmtree(staging, ignore_errors=True)
        raise


def read_export(directory, kind):
    directory = plain(Path(directory))
    record = json.loads(WORKER.read_bounded(directory / "export.json", 1 << 20))
    require(record.get("kind") == kind, f"{directory} is not a {kind} export.")
    for name, entry in record["files"].items():
        require(digest(directory / name) == entry["sha256"], f"Export file {name} changed after it was written.")
    return directory, record


def request(port, path, body=None, timeout=120):
    data = None if body is None else json.dumps(body).encode()
    message = urllib.request.Request(f"http://127.0.0.1:{port}{path}", data=data, headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(message, timeout=timeout) as response:
            return json.loads(response.read())
    except urllib.error.HTTPError as error:
        raise ValueError(f"llama-server {path} returned HTTP {error.code}: {error.read().decode(errors='replace')[:500]}") from None


class Server:
    def __init__(self, executable, model, lora, extra, timeout):
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            self.port = probe.getsockname()[1]
        command = [str(executable), "-m", str(model), "--host", "127.0.0.1", "--port", str(self.port), "--jinja", "-c", "4096",
                   "--no-webui", *extra]
        if lora:
            command += ["--lora", str(lora)]
        # A file, not a pipe: an undrained pipe can stall the server, and the log is read only on failure.
        self.log = tempfile.TemporaryFile()
        self.process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=self.log, stderr=subprocess.STDOUT,
                                        creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        deadline = time.monotonic() + timeout
        while True:
            if self.process.poll() is not None:
                self.log.seek(0)
                output = self.log.read().decode(errors="replace")[-2000:]
                self.log.close()
                raise ValueError(f"llama-server exited early: {output}")
            try:
                if request(self.port, "/health", timeout=2).get("status") == "ok":
                    return
            except Exception:
                pass
            if time.monotonic() >= deadline:
                self.close()
                raise ValueError("llama-server did not become ready.")
            time.sleep(0.25)

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        self.log.close()


def compare(server, tokenizer, template_text):
    checks = {}
    props = request(server.port, "/props")
    checks["template"] = props.get("chat_template") == template_text
    checks["endOfTurn"] = props.get("eos_token") == tokenizer.eos_token
    rendering, tokens, replies = True, True, []
    for messages, tools in [(sample, None) for sample in SAMPLES] + [(SAMPLES[0], TOOLS)]:
        body = {"messages": messages, "chat_template_kwargs": {"enable_thinking": False}}
        if tools:
            body["tools"] = tools
        served = request(server.port, "/apply-template", body)["prompt"]
        expected = tokenizer.apply_chat_template(messages, tools=tools, tokenize=False, add_generation_prompt=True, enable_thinking=False)
        rendering = rendering and served == expected
        served_ids = request(server.port, "/tokenize", {"content": served, "add_special": False, "parse_special": True})["tokens"]
        tokens = tokens and served_ids == tokenizer(served, add_special_tokens=False)["input_ids"]
        # Raw completion of the exact rendered prompt: the chat endpoint's reasoning and tool-call parser would
        # otherwise reshape or reject output, and base and adapter are compared as the model produced them.
        # Informational only: the server's output-format parser can reject text that is not a well-formed reply.
        try:
            reply = request(server.port, "/completion", {"prompt": served, "n_predict": 8, "temperature": 0, "cache_prompt": False})
            replies.append({"content": reply.get("content", ""), "stop": reply.get("stop_type") or ("eos" if reply.get("stopped_eos") else "limit")})
        except ValueError as error:
            replies.append({"error": str(error)})
    checks["rendering"], checks["tokenizer"] = rendering, tokens
    return checks, replies


def command_check(arguments):
    base_directory, base_export = read_export(arguments.base_export, "base")
    adapter = None
    if arguments.adapter_export:
        adapter_directory, adapter_export = read_export(arguments.adapter_export, "adapter")
        require(adapter_export["base"] == base_export["base"], "The adapter export belongs to a different base export.")
        adapter = adapter_directory / "adapter-f16.gguf"
    base_dir = plain(Path(arguments.base))
    require(WORKER.digest(base_dir / "chat_template.jinja") == base_export["base"]["templateSha256"],
            "The Hugging Face base is not the one the export was made from.")
    template_text = (base_dir / "chat_template.jinja").read_text(encoding="utf-8")
    # Imported only after every input is verified, so a refusal never needs the runtime.
    from transformers import AutoTokenizer

    tokenizer = AutoTokenizer.from_pretrained(base_dir, local_files_only=True)
    model = base_directory / f"model-{base_export['quantization']}.gguf"
    identity = base_export["gguf"][model.name]
    record = {"kind": "check", "base": base_export["base"], "model": model.name, "adapter": bool(adapter),
              "embeddedTemplate": identity["chatTemplateSha256"] == hashlib.sha256(template_text.encode()).hexdigest(),
              "architecture": identity["architecture"], "blockCount": identity["blockCount"],
              "layersMatch": identity["blockCount"] == base_export["base"]["layers"] + identity["nextnLayers"]}
    runs = [("base", None)] + ([("adapter", adapter)] if adapter else [])
    for name, lora in runs:
        server = Server(arguments.server, model, lora, arguments.server_arg or [], arguments.timeout)
        try:
            record[name] = dict(zip(("checks", "replies"), compare(server, tokenizer, template_text)))
        finally:
            server.close()
    if adapter:
        produced = all("content" in reply for name in ("base", "adapter") for reply in record[name]["replies"])
        record["adapterChangedReplies"] = record["adapter"]["replies"] != record["base"]["replies"] if produced else None
    gates = [record["embeddedTemplate"], record["layersMatch"]] + [all(record[name]["checks"].values()) for name, _ in runs]
    record["passed"] = all(gates)
    output = plain(Path(arguments.output).absolute())
    require(not output.exists(), f"{output} already exists; a check is never replaced.")
    output.write_text(json.dumps(record, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return record


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("base", "adapter"):
        command = commands.add_parser(name)
        command.add_argument("--base", required=True)
        command.add_argument("--repository", required=True)
        command.add_argument("--revision", required=True)
        command.add_argument("--install-manifest-sha256", required=True)
        command.add_argument("--llama-cpp", required=True, help="The pinned llama.cpp source checkout with its converter scripts.")
        command.add_argument("--converter-python", required=True)
        command.add_argument("--output", required=True)
        command.add_argument("--timeout", type=int, default=7200)
        if name == "base":
            command.add_argument("--quantize", required=True, help="llama-quantize from the same llama.cpp build.")
            command.add_argument("--quantization", default="Q4_K_M")
        else:
            command.add_argument("--candidate", required=True)
    check = commands.add_parser("check")
    check.add_argument("--base", required=True, help="The verified Hugging Face base, for its tokenizer and template.")
    check.add_argument("--base-export", required=True)
    check.add_argument("--adapter-export")
    check.add_argument("--server", required=True)
    check.add_argument("--server-arg", action="append", help="Extra llama-server argument, for example --server-arg=-ngl --server-arg=99.")
    check.add_argument("--output", required=True)
    check.add_argument("--timeout", type=int, default=300)
    arguments = parser.parse_args(argv)
    try:
        record = {"base": command_base, "adapter": command_adapter, "check": command_check}[arguments.command](arguments)
    except (ValueError, subprocess.TimeoutExpired) as error:
        print(json.dumps({"state": "failed", "error": str(error)}), file=sys.stderr)
        return 1
    print(json.dumps({"state": "complete", "command": arguments.command, "passed": record.get("passed", True)}))
    return 0 if record.get("passed", True) else 2


if __name__ == "__main__":
    sys.exit(main())
