"""One optional offline LoRA job for a reviewed dataset export.

Nothing here runs at startup and nothing is downloaded. The worker never opens the test
partition, rechecks withdrawals before it reads an example, and publishes only a
complete candidate whose manifest binds the base checkpoint, the dataset export and the
exact configuration. Promotion is a separate decision. The native owner controls the
process tree and deadline, and asks for cancellation by creating `cancel` in the job
directory.

`prepare` writes a new job for a dataset export and an installed base, with every digest
taken from the files themselves and checked as a run would check them.
"""

from __future__ import annotations

import argparse
import functools
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import random
import re
import shutil
import stat
import sys


SCHEMA = "revia-training-job-v1"
DATASET_SCHEMA = "revia-dataset-v1"
CANDIDATE_SCHEMA = "revia-adapter-candidate-v1"
INSTALL_MANIFEST = "revia-install-manifest.json"
MIB = 1024 ** 2
SMALL_FILE = 64 * MIB


class Cancelled(Exception):
    pass


def require(condition, message):
    if not condition:
        raise ValueError(message)


def digest_bytes(data):
    return hashlib.sha256(data).hexdigest()


def digest(path):
    value = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(MIB), b""):
            value.update(block)
    return value.hexdigest()


def plain(path):
    path = Path(path)
    require(not str(path).startswith(("\\\\", "//")), "Network paths are not training inputs or destinations.")
    for part in (path, *path.parents):
        if part.exists():
            information = part.lstat()
            require(not stat.S_ISLNK(information.st_mode) and not getattr(information, "st_file_attributes", 0) & 0x400,
                    "Links and reparse points are not training inputs or destinations.")
    return path


def outside_onedrive(path):
    """Checkpoints and adapters come from private data; OneDrive would upload them."""
    resolved = Path(path).resolve()
    for variable in ("OneDrive", "OneDriveConsumer", "OneDriveCommercial"):
        root = os.environ.get(variable)
        if root and (resolved == Path(root).resolve() or Path(root).resolve() in resolved.parents):
            raise ValueError("Training scratch and candidates stay outside OneDrive; choose a local folder such as D:/ReviaTraining.")
    return path


def read_bounded(path, limit):
    """Every dataset and manifest byte the worker reads comes through here."""
    path = plain(path)
    with path.open("rb") as stream:
        data = stream.read(limit + 1)
    require(len(data) <= limit, f"{path.name} exceeds its bound.")
    return data


def read_json(path, limit=SMALL_FILE):
    return json.loads(read_bounded(path, limit))


def free_ram_mib():
    if os.name == "nt":
        import ctypes
        from ctypes import wintypes

        class MemoryStatus(ctypes.Structure):
            _fields_ = [("length", wintypes.DWORD), ("load", wintypes.DWORD)] + [
                (name, ctypes.c_ulonglong) for name in ("total", "free", "totalPage", "freePage", "totalVirtual", "freeVirtual", "extended")]

        status = MemoryStatus()
        status.length = ctypes.sizeof(status)
        require(ctypes.WinDLL("kernel32").GlobalMemoryStatusEx(ctypes.byref(status)), "System RAM could not be measured.")
        return status.free / MIB
    for line in Path("/proc/meminfo").read_text().splitlines():
        if line.startswith("MemAvailable:"):
            return int(line.split()[1]) / 1024
    raise ValueError("System RAM could not be measured.")


def free_disk_mib(path):
    return shutil.disk_usage(path).free / MIB


def emit(state, **values):
    print(json.dumps({"state": state, **values}, allow_nan=False), flush=True)


def bounded_int(value, low, high):
    return type(value) is int and low <= value <= high


def validate_configuration(configuration):
    require(isinstance(configuration, dict), "The training configuration must be an object.")
    precision = configuration.get("precision")
    require(precision not in ("qlora", "4bit", "int4", "int8", "nf4"),
            "Quantized LoRA is refused: Qwen3.5 guidance recommends BF16 LoRA because 4-bit training shows larger than normal quantization differences.")
    require(configuration.get("method") == "sft-lora" and precision == "bf16", "Only BF16 supervised LoRA is supported.")
    require(configuration.get("enableThinking") is False,
            "Only non-reasoning turns are qualified: with thinking on, the template does not render the prompt as a prefix of the reply.")
    require(configuration.get("backend") in BACKENDS, "Unknown training backend.")
    require(bounded_int(configuration.get("seed"), 0, 0xffffffff), "A uint32 seed is required.")
    require(bounded_int(configuration.get("maxSteps"), 1, 100000), "maxSteps must be from 1 through 100,000.")
    learning_rate = configuration.get("learningRate")
    require(type(learning_rate) in (int, float) and 0 < learning_rate <= 0.01, "learningRate must be above 0 and at most 0.01.")
    require(bounded_int(configuration.get("gradientAccumulation"), 1, 256), "gradientAccumulation must be from 1 through 256.")
    require(bounded_int(configuration.get("maxSequenceTokens"), 64, 32768), "maxSequenceTokens must be from 64 through 32,768.")
    require(bounded_int(configuration.get("loraRank"), 1, 256) and bounded_int(configuration.get("loraAlpha"), 1, 1024),
            "LoRA rank and alpha are out of bounds.")
    dropout = configuration.get("loraDropout")
    require(type(dropout) in (int, float) and 0 <= dropout <= 0.5, "loraDropout must be from 0 through 0.5.")
    modules = configuration.get("targetModules")
    require(isinstance(modules, list) and 0 < len(modules) <= 32 and
            all(isinstance(name, str) and re.fullmatch(r"[A-Za-z0-9_.]{1,64}", name) for name in modules),
            "targetModules must name 1 to 32 modules.")
    require(bounded_int(configuration.get("checkpointEverySteps"), 1, configuration["maxSteps"]),
            "checkpointEverySteps must be within maxSteps.")


def validate_job(job, job_path):
    require(job.get("schema") == SCHEMA, "Unsupported training job schema.")
    identifier = job.get("jobId")
    require(isinstance(identifier, str) and re.fullmatch(r"[A-Za-z0-9_-]{1,96}", identifier), "Invalid training job identity.")
    scratch = outside_onedrive(plain(Path(job.get("scratchRoot", ""))))
    output_root = outside_onedrive(plain(Path(job.get("outputRoot", ""))))
    directory = scratch / identifier
    require(scratch.is_absolute() and output_root.is_absolute() and scratch.name == "ReviaTrainingJobs" and
            Path(job_path) == directory / "job.json" and not scratch.is_relative_to(output_root) and
            not output_root.is_relative_to(scratch), "The training job escaped its separate owned scratch directory.")
    output = output_root / f"adapter-{identifier}"
    require(not output.exists() and not (output_root / f".adapter-{identifier}.partial").exists(),
            "The candidate destination already exists; a candidate is never replaced.")
    base, dataset, resources = job.get("base"), job.get("dataset"), job.get("resources")
    require(isinstance(base, dict) and re.fullmatch(r"[0-9a-f]{40}", str(base.get("revision"))) and
            re.fullmatch(r"[0-9a-f]{64}", str(base.get("installManifestSha256"))) and isinstance(base.get("repository"), str) and
            re.fullmatch(r"[A-Za-z0-9_.-]{1,96}/[A-Za-z0-9_.-]{1,96}", base["repository"]),
            "The base checkpoint needs a repository, a pinned revision and its install manifest digest.")
    require(isinstance(dataset, dict) and re.fullmatch(r"[0-9a-f]{64}", str(dataset.get("manifestSha256"))),
            "The dataset export needs its manifest digest.")
    require(isinstance(job.get("withdrawnSourcesPath"), str), "The current withdrawal list is required.")
    require(isinstance(resources, dict) and re.fullmatch(r"cpu|cuda:[0-7]", str(resources.get("device"))) and
            all(bounded_int(resources.get(key), 0, 1 << 20) for key in ("minimumFreeVramMiB", "minimumFreeRamMiB", "minimumFreeDiskMiB")),
            "The job needs a device and its free VRAM, RAM and disk floors.")
    validate_configuration(job.get("configuration"))
    job["directory"], job["outputPath"] = str(directory), str(output)
    return job


def verify_base(job):
    base = job["base"]
    directory = plain(Path(base["directory"]))
    manifest_bytes = read_bounded(directory / INSTALL_MANIFEST, SMALL_FILE)
    require(digest_bytes(manifest_bytes) == base["installManifestSha256"], "The base install manifest does not match the job.")
    manifest = json.loads(manifest_bytes)
    require(manifest.get("repository") == base["repository"] and manifest.get("revision") == base["revision"],
            "The installed base is a different repository or revision.")
    files = manifest.get("files")
    require(isinstance(files, dict) and "config.json" in files and "chat_template.jinja" in files, "The base install manifest is incomplete.")
    for name, expected in files.items():
        path = plain(directory / name)
        require(re.fullmatch(r"[A-Za-z0-9_.-]{1,128}", name) and path.is_file() and path.stat().st_size == expected.get("bytes"),
                f"Installed base file {name} is missing or has the wrong size.")
        # Shards were hashed at installation; the small files that decide behavior are rehashed.
        if expected["bytes"] <= SMALL_FILE:
            require(digest(path) == expected.get("sha256"), f"Installed base file {name} changed since installation.")
    config = json.loads(read_bounded(directory / "config.json", SMALL_FILE))
    text = config.get("text_config", config)
    return {"repository": base["repository"], "revision": base["revision"], "installManifestSha256": base["installManifestSha256"],
            "architectures": config.get("architectures", []), "modelType": text.get("model_type", config.get("model_type", "")),
            "hiddenSize": text.get("hidden_size"), "layers": text.get("num_hidden_layers"), "vocabSize": text.get("vocab_size"),
            "templateSha256": files["chat_template.jinja"]["sha256"]}


def served_tools(tools):
    """The offered tools as the pinned llama-server hands them to the chat template.

    It rebuilds each one as type, then function with name, description and parameters, and
    fills in an empty description; parameters keep the order they were sent in. Rendering the
    exported order instead would train on a prompt the server never produced.
    """
    if tools is None:
        return None
    require(isinstance(tools, list) and tools, "An example's tools must be a non-empty list.")
    served = []
    for tool in tools:
        function = tool.get("function") if isinstance(tool, dict) and tool.get("type") == "function" else None
        require(isinstance(function, dict) and isinstance(function.get("name"), str) and function["name"],
                "An example offers a tool that is not a named function.")
        served.append({"type": "function", "function": {"name": function["name"], "description": function.get("description", ""),
                                                         "parameters": function.get("parameters", {})}})
    return served


def load_partition(directory, name, entry, limit=1 << 30):
    data = read_bounded(directory / name, limit)
    require(digest_bytes(data) == entry.get("sha256"), f"{name} does not match its dataset manifest.")
    records = [json.loads(line) for line in data.decode("utf-8").splitlines() if line]
    require(len(records) == entry.get("examples"), f"{name} does not hold the examples its manifest lists.")
    for record in records:
        messages = record.get("messages")
        require(isinstance(messages, list) and len(messages) >= 2 and messages[-1].get("role") == "assistant" and
                all(isinstance(message.get("content"), str) and message.get("role") in ("system", "user", "assistant", "tool")
                    for message in messages), f"{name} holds an example that does not end with its supervised reply.")
        record["tools"] = served_tools(record.get("tools"))
    return records


def verify_dataset(job):
    directory = plain(Path(job["dataset"]["directory"]))
    manifest_bytes = read_bounded(directory / "manifest.json", SMALL_FILE)
    require(digest_bytes(manifest_bytes) == job["dataset"]["manifestSha256"], "The dataset manifest does not match the job.")
    manifest = json.loads(manifest_bytes)
    require(manifest.get("schema") == DATASET_SCHEMA, "Unsupported dataset export schema.")
    withdrawn_bytes = read_bounded(Path(job["withdrawnSourcesPath"]), SMALL_FILE)
    withdrawn = json.loads(withdrawn_bytes)
    require(isinstance(withdrawn, list) and all(isinstance(item, str) for item in withdrawn), "The withdrawal list must be strings.")
    withdrawn = set(withdrawn)
    stale = [example["id"] for example in manifest.get("examples", [])
             if example.get("eligible") and withdrawn.intersection(example.get("sources", []))]
    require(not stale, f"{len(stale)} exported examples use sources withdrawn since the export; prepare a new export.")
    files = manifest.get("files", {})
    # The test partition stays held out: it is neither read nor hashed here.
    train = load_partition(directory, "train.jsonl", files.get("train.jsonl", {}))
    validation = load_partition(directory, "validation.jsonl", files.get("validation.jsonl", {}))
    require(train, "The dataset export has no training examples.")
    identity = {"name": manifest.get("name"), "manifestSha256": job["dataset"]["manifestSha256"], "trainExamples": len(train),
                "validationExamples": len(validation), "withdrawnSourcesSha256": digest_bytes(withdrawn_bytes)}
    return train, validation, identity


def check_resources(job, backend, phase):
    resources = job["resources"]
    require(free_ram_mib() >= resources["minimumFreeRamMiB"], f"Free system RAM is below the job's floor {phase}.")
    if phase == "before loading":
        require(free_disk_mib(job["outputRoot"]) >= resources["minimumFreeDiskMiB"], "Free disk is below the job's floor.")
        vram = backend.free_vram_mib()
        require(vram is None or vram >= resources["minimumFreeVramMiB"], "Free VRAM on the selected device is below the job's floor.")


@functools.lru_cache(maxsize=2)
def epoch_order(count, seed, epoch):
    order = list(range(count))
    random.Random(seed * 1000003 + epoch).shuffle(order)
    return tuple(order)


def schedule(count, seed, step, accumulation):
    """The examples for one optimizer step depend only on seed and step, so resuming continues the same order."""
    picks = []
    for micro in range(accumulation):
        epoch, offset = divmod(step * accumulation + micro, count)
        picks.append(epoch_order(count, seed, epoch)[offset])
    return picks


def save_checkpoint(job, backend, identity, step):
    directory = Path(job["directory"])
    target = directory / f"checkpoint-{step:06d}"
    if not target.exists():
        backend.save_checkpoint(target)
    pointer = directory / "checkpoint.json.partial"
    pointer.write_text(json.dumps({"identity": identity, "step": step, "directory": target.name}), encoding="utf-8")
    os.replace(pointer, directory / "checkpoint.json")
    for older in directory.glob("checkpoint-*"):
        if older.is_dir() and older.name != target.name:
            shutil.rmtree(older)


def resume_step(job, backend, identity):
    pointer = Path(job["directory"]) / "checkpoint.json"
    if not pointer.exists():
        return 0
    saved = json.loads(read_bounded(pointer, 65536))
    require(saved.get("identity") == identity,
            "This job directory holds a checkpoint from a different base, dataset or configuration.")
    backend.load_checkpoint(Path(job["directory"]) / saved["directory"])
    return saved["step"]


def publish(job, backend, record):
    final = Path(job["outputPath"])
    staging = final.parent / f".{final.name}.partial"
    require(not final.exists() and not staging.exists(), "The candidate destination already exists; a candidate is never replaced.")
    staging.mkdir(parents=True)
    try:
        backend.save_adapter(staging)
        files = {}
        for path in sorted(staging.iterdir()):
            require(path.is_file() and not path.is_symlink(), "An adapter must be plain files.")
            files[path.name] = {"sha256": digest(path), "bytes": path.stat().st_size}
        require(any(name.endswith(".safetensors") for name in files), "The backend saved no adapter weights.")
        record["files"] = files
        (staging / "candidate.json").write_text(json.dumps(record, indent=2, sort_keys=True, allow_nan=False) + "\n", encoding="utf-8")
        os.rename(staging, final)
    except BaseException:
        shutil.rmtree(staging, ignore_errors=True)
        raise
    return digest(final / "candidate.json")


def run(job, backend_factory=None):
    emit("validating")
    base = verify_base(job)
    train, validation, dataset = verify_dataset(job)
    configuration = job["configuration"]
    identity = digest_bytes(json.dumps({"base": base, "dataset": dataset, "configuration": configuration}, sort_keys=True).encode())
    backend = (backend_factory or BACKENDS[configuration["backend"]])(configuration, job["resources"]["device"])
    check_resources(job, backend, "before loading")
    cancel = Path(job["directory"]) / "cancel"
    emit("loading")
    backend.load(Path(job["base"]["directory"]))
    encoded = [backend.encode(record["messages"], record["tools"]) for record in train]
    held = [backend.encode(record["messages"], record["tools"]) for record in validation]
    step = resume_step(job, backend, identity)
    resumed = step
    loss = None
    while step < configuration["maxSteps"]:
        if cancel.exists():
            save_checkpoint(job, backend, identity, step)
            raise Cancelled(step)
        picks = schedule(len(encoded), configuration["seed"], step, configuration["gradientAccumulation"])
        loss = backend.step([encoded[index] for index in picks])
        require(math.isfinite(loss), f"Training loss became non-finite at step {step + 1}.")
        step += 1
        check_resources(job, backend, "during training")
        emit("training", step=step, totalSteps=configuration["maxSteps"], loss=loss)
        if step % configuration["checkpointEverySteps"] == 0 and step < configuration["maxSteps"]:
            save_checkpoint(job, backend, identity, step)
    emit("evaluating", step=step)
    validation_loss = backend.evaluate(held) if held else None
    emit("publishing", step=step)
    record = {"schema": CANDIDATE_SCHEMA, "state": "candidate", "jobId": job["jobId"], "base": base, "dataset": dataset,
              "configuration": configuration,
              "configurationSha256": digest_bytes(json.dumps(configuration, sort_keys=True).encode()),
              "template": {"sha256": base["templateSha256"], "enableThinking": False, "masking": "assistant reply only"},
              "metrics": {"steps": step, "finalTrainLoss": loss, "validationLoss": validation_loss, "resumedFromStep": resumed,
                          # Measured, so a short qualification run shows whether the configuration fits the card.
                          "peakVramMiB": getattr(backend, "peak_vram_mib", lambda: None)()},
              "environment": {"python": platform.python_version(), "platform": platform.platform(), **backend.versions()}}
    candidate_sha = publish(job, backend, record)
    return {"state": "complete", "jobId": job["jobId"], "candidatePath": job["outputPath"], "candidateManifestSha256": candidate_sha,
            "steps": step, "resumedFromStep": resumed, "validationLoss": validation_loss}


class TransformersPeftBackend:
    """BF16 LoRA with transformers and peft, imported only when a job asks for it."""

    def __init__(self, configuration, device):
        try:
            import peft
            import safetensors.torch
            import torch
            import transformers
        except ImportError as error:
            raise ValueError(f"The optional training runtime is not installed: {error.name}.") from None
        self.peft, self.safetensors, self.torch, self.transformers = peft, safetensors.torch, torch, transformers
        self.configuration, self.device = configuration, device

    def versions(self):
        return {"torch": self.torch.__version__, "transformers": self.transformers.__version__, "peft": self.peft.__version__}

    def free_vram_mib(self):
        if not self.device.startswith("cuda"):
            return None
        free, _ = self.torch.cuda.mem_get_info(int(self.device.split(":")[1]))
        return free / MIB

    def peak_vram_mib(self):
        if not self.device.startswith("cuda"):
            return None
        return self.torch.cuda.max_memory_allocated(int(self.device.split(":")[1])) / MIB

    def load(self, directory):
        torch, configuration = self.torch, self.configuration
        torch.manual_seed(configuration["seed"])
        self.tokenizer = self.transformers.AutoTokenizer.from_pretrained(directory, local_files_only=True)
        model = self.transformers.AutoModelForCausalLM.from_pretrained(directory, local_files_only=True, dtype=torch.bfloat16)
        model.config.use_cache = False
        if hasattr(model, "gradient_checkpointing_enable"):
            model.gradient_checkpointing_enable()
            model.enable_input_require_grads()
        lora = self.peft.LoraConfig(r=configuration["loraRank"], lora_alpha=configuration["loraAlpha"],
                                    lora_dropout=configuration["loraDropout"], target_modules=configuration["targetModules"],
                                    task_type="CAUSAL_LM")
        self.model = self.peft.get_peft_model(model, lora).to(self.device)
        self.trainable = [parameter for parameter in self.model.parameters() if parameter.requires_grad]
        self.optimizer = torch.optim.AdamW(self.trainable, lr=configuration["learningRate"])

    def _ids(self, messages, **options):
        value = self.tokenizer.apply_chat_template(messages, tokenize=True, enable_thinking=False, **options)
        return list(value["input_ids"] if hasattr(value, "keys") else value)

    def encode(self, messages, tools=None):
        options = {"tools": tools} if tools else {}
        prompt = self._ids(messages[:-1], add_generation_prompt=True, **options)
        full = self._ids(messages, **options)
        require(len(full) > len(prompt) and full[:len(prompt)] == prompt,
                "The chat template does not render an example's prompt as a prefix of its conversation, so its reply cannot be masked exactly.")
        require(len(full) <= self.configuration["maxSequenceTokens"], "An example exceeds maxSequenceTokens; it is refused rather than truncated.")
        return full, [-100] * len(prompt) + full[len(prompt):]

    def _loss(self, example):
        tokens, labels = example
        tensor = self.torch.tensor
        return self.model(input_ids=tensor([tokens], device=self.device), labels=tensor([labels], device=self.device)).loss

    def step(self, examples):
        self.model.train()
        total = 0.0
        for example in examples:
            loss = self._loss(example)
            (loss / len(examples)).backward()
            total += loss.item()
        self.torch.nn.utils.clip_grad_norm_(self.trainable, 1.0)
        self.optimizer.step()
        self.optimizer.zero_grad(set_to_none=True)
        return total / len(examples)

    def evaluate(self, examples):
        self.model.eval()
        with self.torch.no_grad():
            return sum(self._loss(example).item() for example in examples) / len(examples)

    def save_checkpoint(self, directory):
        self.model.save_pretrained(directory / "adapter")
        self.torch.save({"optimizer": self.optimizer.state_dict(), "rng": self.torch.get_rng_state()}, directory / "state.pt")

    def load_checkpoint(self, directory):
        weights = self.safetensors.load_file(str(directory / "adapter" / "adapter_model.safetensors"), device=self.device)
        self.peft.set_peft_model_state_dict(self.model, weights)
        state = self.torch.load(directory / "state.pt", weights_only=True)
        self.optimizer.load_state_dict(state["optimizer"])
        self.torch.set_rng_state(state["rng"])

    def save_adapter(self, directory):
        self.model.save_pretrained(directory)


BACKENDS = {"transformers-peft": TransformersPeftBackend}


def finish(directory, receipt):
    directory = Path(directory)
    target = directory / "result.json"
    if target.exists():
        # Each attempt keeps its own result; a resumed job retains why the last one stopped.
        attempts = len(list(directory.glob("result-*.json")))
        os.rename(target, directory / f"result-{attempts + 1}.json")
    with target.open("x", encoding="utf-8") as output:
        json.dump(receipt, output, allow_nan=False)


# The 4B experiment's configuration in docs/OFFLINE_TRAINING.md. A smoke run measures peak VRAM
# in 5 steps; a full run is three epochs.
PREPARED_CONFIGURATION = {
    "method": "sft-lora", "backend": "transformers-peft", "precision": "bf16", "enableThinking": False, "seed": 20261031,
    "learningRate": 0.0001, "gradientAccumulation": 8, "maxSequenceTokens": 4096, "loraRank": 16, "loraAlpha": 32,
    "loraDropout": 0.05, "targetModules": ["q_proj", "k_proj", "v_proj", "o_proj", "gate_proj", "up_proj", "down_proj"]}
SMOKE_STEPS = 5
EPOCHS = 3


def prepare(arguments):
    """Writes a new training job for a dataset export and an installed base, checked as a run checks it."""
    base = plain(Path(arguments.base).absolute())
    install_bytes = read_bounded(base / INSTALL_MANIFEST, SMALL_FILE)
    installed = json.loads(install_bytes)
    dataset = plain(Path(arguments.dataset).absolute())
    manifest_bytes = read_bounded(dataset / "manifest.json", SMALL_FILE)
    manifest = json.loads(manifest_bytes)
    require(manifest.get("schema") == DATASET_SCHEMA, "Unsupported dataset export schema.")
    train_examples = manifest.get("files", {}).get("train.jsonl", {}).get("examples")
    require(bounded_int(train_examples, 1, 1 << 30), "The dataset export has no training examples.")
    accumulation = PREPARED_CONFIGURATION["gradientAccumulation"]
    steps = SMOKE_STEPS if arguments.smoke else math.ceil(EPOCHS * train_examples / accumulation)
    configuration = {**PREPARED_CONFIGURATION, "maxSequenceTokens": arguments.max_sequence_tokens, "maxSteps": steps,
                     "checkpointEverySteps": min(steps, SMOKE_STEPS)}
    scratch = Path(arguments.scratch_root).absolute()
    job_path = scratch / arguments.job_id / "job.json"
    job = {"schema": SCHEMA, "jobId": arguments.job_id, "scratchRoot": str(scratch), "outputRoot": str(Path(arguments.output_root).absolute()),
           "withdrawnSourcesPath": str(Path(arguments.withdrawn).absolute()),
           "base": {"directory": str(base), "repository": installed.get("repository"), "revision": installed.get("revision"),
                    "installManifestSha256": digest_bytes(install_bytes)},
           "dataset": {"directory": str(dataset), "manifestSha256": digest_bytes(manifest_bytes)},
           "resources": {"device": arguments.device, "minimumFreeVramMiB": arguments.minimum_free_vram_mib,
                         "minimumFreeRamMiB": arguments.minimum_free_ram_mib, "minimumFreeDiskMiB": arguments.minimum_free_disk_mib},
           "configuration": configuration}
    require(not job_path.exists(), f"{job_path} already exists; a job is never replaced. Use a new --job-id.")
    # Every check a run makes before loading a model, so a prepared job does not fail on them later.
    checked = json.loads(json.dumps(job))
    validate_job(checked, job_path)
    verify_base(checked)
    train, validation, _ = verify_dataset(checked)
    job_path.parent.mkdir(parents=True, exist_ok=True)
    job_path.write_bytes((json.dumps(job, indent=2) + "\n").encode("utf-8"))
    return {"job": str(job_path), "maxSteps": steps, "trainExamples": len(train), "validationExamples": len(validation),
            "base": f"{installed.get('repository')}@{installed.get('revision')}"}


def main_prepare(argv):
    parser = argparse.ArgumentParser(prog="revia_training_worker.py prepare", description=prepare.__doc__)
    parser.add_argument("--dataset", required=True, help="A dataset export directory.")
    parser.add_argument("--base", required=True, help="An installed base directory with its revia-install-manifest.json.")
    parser.add_argument("--job-id", required=True)
    parser.add_argument("--scratch-root", required=True, help="An owned folder named ReviaTrainingJobs.")
    parser.add_argument("--output-root", required=True, help="Where the candidate adapter is published.")
    parser.add_argument("--withdrawn", required=True, help="The current withdrawal list, such as the companion's withdrawn.json.")
    parser.add_argument("--smoke", action="store_true", help=f"A {SMOKE_STEPS}-step run that measures peak VRAM.")
    parser.add_argument("--max-sequence-tokens", type=int, default=PREPARED_CONFIGURATION["maxSequenceTokens"])
    parser.add_argument("--device", default="cuda:0")
    parser.add_argument("--minimum-free-vram-mib", type=int, default=10500)
    parser.add_argument("--minimum-free-ram-mib", type=int, default=16384)
    parser.add_argument("--minimum-free-disk-mib", type=int, default=20480)
    arguments = parser.parse_args(argv)
    try:
        result = prepare(arguments)
    except (ValueError, KeyError, OSError, json.JSONDecodeError) as error:
        print(json.dumps({"state": "failed", "error": f"{type(error).__name__}: {error}"}), file=sys.stderr)
        return 1
    print(json.dumps({"state": "prepared", **result}, indent=2))
    return 0


def main(argv=None, backend_factory=None):
    argv = sys.argv[1:] if argv is None else argv
    if argv and argv[0] == "prepare":
        return main_prepare(argv[1:])
    parser = argparse.ArgumentParser()
    parser.add_argument("--job", required=True)
    arguments = parser.parse_args(argv)
    job_path = Path(arguments.job).absolute()
    directory = job_path.parent
    previous = directory / "result.json"
    if previous.exists() and json.loads(read_bounded(previous, 65536)).get("state") == "complete":
        emit("failed", error="This training job already completed.")
        return 1
    job = {}
    try:
        job = read_json(plain(job_path), 65536)
        require(isinstance(job, dict), "A training job must be a JSON object.")
        validate_job(job, job_path)
        receipt = run(job, backend_factory)
    except Cancelled as stopped:
        finish(directory, {"state": "cancelled", "jobId": job.get("jobId"), "step": stopped.args[0]})
        emit("cancelled", step=stopped.args[0])
        return 3
    except Exception as error:
        finish(directory, {"state": "failed", "jobId": job.get("jobId"), "error": str(error)})
        emit("failed", error=str(error))
        return 1
    finish(directory, receipt)
    emit("complete", step=receipt["steps"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
