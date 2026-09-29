#!/usr/bin/env python3
"""Trains a persona adapter (QLoRA) on Revia's exported conversations with Unsloth.

    python train_persona_lora.py --base unsloth/Qwen3-8B-unsloth-bnb-4bit \
        --dataset RuntimeData/Learning/persona.jsonl --out RuntimeData/Learning/adapter-v1 \
        [--rank 16] [--epochs 2] [--learning-rate 2e-4] [--max-seq 2048] [--dry-run]

The top rung of the ladder, and the one with the documented risks: an adapter trained
on a person's own conversations can learn to please rather than to be honest. So this
script produces nothing Revia will load by itself. It writes the adapter and an
adapter_manifest.json naming the base model, the dataset's SHA-256 and the settings;
gate_adapter.py must then pass before the adapter is put on llama-server's --lora,
and taking it off that line is the whole of rolling it back.

--dry-run checks the dataset, counts examples and approximate tokens, and prints the
plan without importing any training library, so the shape of a run can be checked on
a machine with no GPU.
"""
import argparse
import hashlib
import json
import os
import sys
import time


def read_dataset(path):
    examples = []
    with open(path, "r", encoding="utf-8") as handle:
        for number, line in enumerate(handle, 1):
            line = line.strip()
            if not line:
                continue
            try:
                example = json.loads(line)
            except json.JSONDecodeError as error:
                raise SystemExit(f"{path}:{number}: not JSON ({error})")
            messages = example.get("messages")
            if not isinstance(messages, list) or len(messages) < 2:
                raise SystemExit(f"{path}:{number}: needs a messages list of at least two turns")
            for message in messages:
                if message.get("role") not in ("system", "user", "assistant") or not isinstance(message.get("content"), str):
                    raise SystemExit(f"{path}:{number}: each message needs a role of system, user or assistant and a string content")
            if messages[-1]["role"] != "assistant":
                raise SystemExit(f"{path}:{number}: the last turn must be the assistant's")
            examples.append(example)
    if not examples:
        raise SystemExit(f"{path}: holds no examples")
    return examples


def file_sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def plan(args, examples):
    characters = sum(len(m["content"]) for e in examples for m in e["messages"])
    return {
        "base_model": args.base,
        "dataset": args.dataset,
        "dataset_sha256": file_sha256(args.dataset),
        "examples": len(examples),
        "approximate_tokens": characters // 4,
        "rank": args.rank,
        "alpha": args.rank * 2,
        "epochs": args.epochs,
        "learning_rate": args.learning_rate,
        "max_seq_length": args.max_seq,
        "output": args.out,
        "method": "qlora-4bit",
    }


def train(args, examples, manifest):
    try:
        from unsloth import FastLanguageModel  # noqa: F401
        from trl import SFTTrainer
        from transformers import TrainingArguments
        from datasets import Dataset
    except ImportError as error:
        raise SystemExit(
            "Training needs unsloth, trl, transformers and datasets in this environment "
            f"(pip install unsloth): {error}")
    model, tokenizer = FastLanguageModel.from_pretrained(
        model_name=args.base, max_seq_length=args.max_seq, load_in_4bit=True)
    model = FastLanguageModel.get_peft_model(
        model, r=args.rank, lora_alpha=args.rank * 2, lora_dropout=0,
        target_modules=["q_proj", "k_proj", "v_proj", "o_proj", "gate_proj", "up_proj", "down_proj"],
        use_gradient_checkpointing="unsloth", random_state=3407)
    texts = [tokenizer.apply_chat_template(example["messages"], tokenize=False) for example in examples]
    dataset = Dataset.from_dict({"text": texts})
    trainer = SFTTrainer(
        model=model, tokenizer=tokenizer, train_dataset=dataset, dataset_text_field="text",
        max_seq_length=args.max_seq,
        args=TrainingArguments(
            per_device_train_batch_size=2, gradient_accumulation_steps=4, num_train_epochs=args.epochs,
            learning_rate=args.learning_rate, logging_steps=10, output_dir=os.path.join(args.out, "checkpoints"),
            optim="adamw_8bit", lr_scheduler_type="cosine", warmup_ratio=0.05, seed=3407, report_to="none"))
    trainer.train()
    model.save_pretrained(args.out)
    tokenizer.save_pretrained(args.out)
    manifest["trained_at"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    manifest["gate"] = "not run: pass gate_adapter.py before use"
    with open(os.path.join(args.out, "adapter_manifest.json"), "w", encoding="utf-8") as handle:
        json.dump(manifest, handle, indent=2)
    if args.llama_cpp:
        converter = os.path.join(args.llama_cpp, "convert_lora_to_gguf.py")
        print(f"To use it with llama-server: python {converter} --base <base gguf dir> {args.out} "
              f"--outfile {os.path.join(args.out, 'adapter.gguf')}; then add --lora <that file> to llm.serverArguments "
              "only after gate_adapter.py passes.")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--base", required=True)
    parser.add_argument("--dataset", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--rank", type=int, default=16)
    parser.add_argument("--epochs", type=float, default=2.0)
    parser.add_argument("--learning-rate", type=float, default=2e-4)
    parser.add_argument("--max-seq", type=int, default=2048)
    parser.add_argument("--llama-cpp", help="a llama.cpp checkout, to print the GGUF conversion step")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)
    examples = read_dataset(args.dataset)
    manifest = plan(args, examples)
    if args.dry_run:
        print(json.dumps(manifest, indent=2))
        return 0
    os.makedirs(args.out, exist_ok=True)
    train(args, examples, manifest)
    print(json.dumps(manifest, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
