# The learning ladder's top rung: a persona adapter, gated

Revia learns first without touching weights: evidence counters, memories she offers for
review, the playbook (`/playbook`) and the procedure library (`/procedures`). This folder
is the last rung, and the one with the documented risks: a small adapter (QLoRA) trained
on the owner's own conversations, so her turns of phrase and her way with the people she
knows sit in the model rather than only in the prompt. Nothing here is loaded by Revia
on its own. An adapter reaches her only when the gate passes and the owner names it in
`llm.serverArguments`; taking it off that line is the whole of rolling it back.

## 1. Export what she may learn from

```powershell
python .\Tools\Learning\export_training_set.py --archive .\RuntimeData\Memory\revia_conversations.db `
    --out .\RuntimeData\Learning\persona.jsonl --persona .\RuntimeData\Learning\persona.txt
```

The archive holds only her private, local conversations; public Discord, Twitch and
YouTube turns never enter it, and the exporter refuses any turn carrying a public
marker anyway, because Discord's developer policy bars training on that content. A
window that holds anything shaped like a credential, a token, a private key or a card
number is dropped whole, never masked. Duplicates go. The manifest written beside the
dataset records the counts, every reason a window was dropped, and the dataset's
SHA-256, so an adapter can say exactly what it learned from. Advisor notes from Claude,
GPT or Gemini are never archived as her turns, so they are not in the export either;
their terms of service forbid training on them.

`persona.txt` is optional: the system prompt every example gets, ideally her own persona
packet rendered once, so the adapter learns to be her under the prompt she actually runs
with.

## 2. Train

```powershell
python .\Tools\Learning\train_persona_lora.py --base unsloth/Qwen3-8B-unsloth-bnb-4bit `
    --dataset .\RuntimeData\Learning\persona.jsonl --out .\RuntimeData\Learning\adapter-v1 --dry-run
```

`--dry-run` validates the dataset and prints the plan with no training library needed.
Without it, the script needs `pip install unsloth` (which brings trl, transformers and
datasets) and a CUDA GPU; an 8B QLoRA fits in about 6 GB. It writes the adapter and an
`adapter_manifest.json` (base model, dataset SHA-256, settings), marked `gate: not run`.
`--llama-cpp <checkout>` prints the GGUF conversion step for llama-server.

## 3. Gate it

Start llama-server with the adapter (`--lora adapter.gguf`) beside the same base model,
then:

```powershell
python .\Tools\Learning\gate_adapter.py --probes .\Tools\Learning\sycophancy_probes.json `
    --server http://127.0.0.1:8080 --tests .\build\ReviaTests.exe
```

The probes are the failure that matters: pressure to agree with something false, to
flatter, to endorse a bad decision. Each has phrases the answer must not contain and a
stance it must hold; one agreement fails the gate. `--tests` also runs the persona
regression suite (`ReviaTests --persona-packet`). The report says `admitted` only when
every probe and the suite pass. Only then add `--lora <path>` to `llm.serverArguments`
in `settings.json`; remove it to roll back. Keep the base model as it was: the adapter
is a separate file and the base is never overwritten.

## Tests

`python .\Tests\learningTools.test.py` builds a small archive with a private
conversation, a secret, a card number, a public turn and a duplicate; checks the export
keeps the first and drops the rest with reasons; runs the trainer's dry run against it;
and scores canned answers through the gate, honest and sycophantic. No training runs
here, and no adapter has been trained or gated on a real archive yet.
