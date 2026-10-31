# Speculative decoding feasibility

Research checkpoint, 2026-10-31. Revia configures no speculative decoding, and ordinary
decoding remains both the default and the fallback. Nothing here is enabled. The findings
come from upstream llama.cpp source at Revia's pinned tag and from public model metadata,
gathered in a cloud session: no model was loaded or run. Prompt batching
([CONTEXT_BENCHMARK.md](CONTEXT_BENCHMARK.md)) and `llm.parallelRequests` are separate
settings with separate claims.

## What the pinned runtime supports

Revia installs llama.cpp `b10453` (`Tools/ReviaAcceleration.ps1`). At that tag
`--spec-type` accepts `draft-simple`, `draft-eagle3`, `draft-mtp`, `draft-dflash`,
`draft-dspark`, `ngram-simple`, `ngram-map-k`, `ngram-map-k4v`, `ngram-mod` and
`ngram-cache`. Draft settings use the `--spec-draft-*` names (`--spec-draft-model`,
`--spec-draft-n-max`, `--spec-draft-n-min`, `--spec-draft-p-min`, `--spec-draft-device`,
`--spec-draft-ngl`). On the host, confirm the installed binary actually is that build
(`ThirdParty/llama.cpp/revia-backend.json`, `llama-server --version`), and take flag names
from its own `--help` before configuring anything.

Qwen3.5-9B is a hybrid model: three Gated DeltaNet linear-attention layers to each
full-attention layer, 32 layers in all. Its recurrent state cannot be rewound one token
at a time, so `b10453` speculates with checkpoints. It saves state before a draft and
restores it when tokens are rejected (`tools/server/server-context.cpp`: "speculative
decoding will use checkpoints", "accepted N/M draft tokens (restore checkpoint)"). That
restore cost is the main unknown for this model.

At `b10453` the server turns off context shift and cache reuse when a vision projector is
loaded, but no gate on speculation was found. Image turns with speculation enabled are
untested.

## Candidates for Main

| Mode | Status | What it needs |
| --- | --- | --- |
| MTP (`draft-mtp`) | Unavailable with the installed GGUF | The base model has one MTP head (`mtp_num_hidden_layers: 1`). Neither Q4_K_M conversion kept it: the manifest's `unsloth/Qwen3.5-9B-GGUF` file at `3885219b` and LM Studio's current file both have 427 tensors, 32 blocks, no `qwen35.nextn_predict_layers` key and no `nextn` tensors. This was read from the files' GGUF headers. The `b10453` converter can export the head on its own with `--mtp`, as a separate draft GGUF beside the existing target, but only from the official checkpoint. |
| DFlash (`draft-dflash`) | First-party 9B draft exists; not converted or run | `z-lab/Qwen3.5-9B-DFlash` at `5fc3b3d474760f18c516db87d84c37edbfd3ede6`, Apache-2.0, 2.58 GB. It is a 6-layer Qwen3-backbone draft with block size 16, reads target layers 1, 5, 9 … 29, and its base is `Qwen/Qwen3.5-9B`. llama.cpp converts it with `--target-model-dir`, so the draft inherits the target's tokenizer and token embeddings; that also needs the official checkpoint. Whether `b10453` extracts the qwen35 layers it reads is unverified; it aborts with "DFlash: target layer N input not extracted" if not. `Qwen3.5-4B-DFlash` is not a 9B draft. |
| n-gram (`ngram-mod`, `ngram-simple`, `ngram-map-k`, `ngram-cache`) | Can be qualified now; no assets | Drafts come from text already in the context, so the likely gains are on repetitive output, such as a code review restating code or an edit that keeps most of a document. |
| EAGLE-3 | No first-party 9B draft | Only third-party drafts were found, for example `BLR2/Eagle3-Qwen3.5-9B`. They are unreviewed and not proposed. |
| Separate draft model (`draft-simple`) | Not proposed | Fast's 0.8B is the same family, but its acceptance rate is unmeasured, and placing it on another GPU adds a cross-device step each round. |

Published DFlash speedups (up to 4.60x on HumanEval at concurrency 1) were measured on one
B200 in BF16 under SGLang. They say nothing about a Q4_K_M target with checkpointed
recurrent state on a 12 GB RTX 5070. Several third-party "MTP" GGUFs on Hugging Face are
fine-tunes or unreviewed conversions; using one would replace Main and require full
requalification.

The installed Main pair is LM Studio's Q4_K_M with its BF16 projector. The tracked
`Config/model_manifest.json` describes the unsloth files instead. LM Studio's current
Q4_K_M is 5,627,044,256 bytes; compare it with the installed file's size and SHA-256 on the
host before relying on the header result above.

## Prepared download request (not yet approved)

The MTP and DFlash routes both need the official checkpoint, which is also the base the
offline fine-tuning roadmap requires.

| Repository | Revision | Files | Bytes | License |
| --- | --- | --- | --- | --- |
| `Qwen/Qwen3.5-9B` | `c202236235762e1c871ad0ccb60c8ee5ba337b9a` | 4 safetensors shards, index, config, tokenizer, chat template, preprocessor configs | 19,329,393,661 | Apache-2.0 |
| `z-lab/Qwen3.5-9B-DFlash` | `5fc3b3d474760f18c516db87d84c37edbfd3ede6` | `model.safetensors`, `config.json` | 2,584,231,063 | Apache-2.0 |

Both repositories are ungated. Proposed destinations are outside OneDrive, beside the
approved LTX assets: `D:/ReviaModels/Qwen3.5-9B-c202236` and
`D:/ReviaModels/Qwen3.5-9B-DFlash-5fc3b3d`. Conversion uses the `b10453` converter with
`--mtp` (MTP head) and `--target-model-dir` (DFlash); the existing Main GGUF and projector
stay as they are.

Memory plan, to be measured rather than assumed: a draft belongs on Main's GPU, because
it reads Main's hidden states. That card already holds Main's weights, its 0.92 GB
projector, the q8_0 cache for the eight full-attention layers and the BF16 voice worker.
A draft adds its own weights, a small context and checkpoint buffers. The two GPUs are not
a pooled allocation.

## Host qualification plan

1. Confirm the binary and its flags, as above.
2. Start with n-gram modes, which need no download. Use ordinary decoding as the baseline,
   with the same prompts, Main's real settings (projector loaded, q8_0 cache, 32K context)
   and greedy decoding where outputs must match exactly. Record queue delay, prefill, first
   visible token, inter-token delay, decode rate, total latency, the server's
   `draft acceptance rate` and per-mode `statistics` lines, peak VRAM, errors and task
   success. Cover ordinary chat, code generation, review of produced code, an image turn,
   and turns during image or video jobs. Report single-user latency separately from
   concurrent throughput.
3. Only after approval of the download: export the MTP head, convert the DFlash draft, and
   measure each the same way.
4. Enable only a measured configuration through settings with ordinary decoding as the
   fallback, and repeat the measurement after any model, quantization, template or
   material prompt change.
