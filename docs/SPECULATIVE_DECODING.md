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
| MTP (`draft-mtp`) | Unavailable with the installed GGUF | The base model has one MTP head (`mtp_num_hidden_layers: 1`). Neither Q4_K_M conversion kept it: the manifest's `unsloth/Qwen3.5-9B-GGUF` file at `3885219b` and LM Studio's current file both have 427 tensors, 32 blocks, no `qwen35.nextn_predict_layers` key and no `nextn` tensors. This was read from the files' GGUF headers. The `b10453` converter can export the head on its own with `--mtp`, as a separate draft GGUF beside the existing target, but only from the official checkpoint. Converting the whole official checkpoint with that converter keeps the head inside the target GGUF; `Tools/revia_adapter_export.py base` records it as `mtpHead`. A self-converted Main would be a new serving artifact that needs full requalification. |
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

## Approved downloads (2026-10-31)

The owner approved the official checkpoint and the DFlash draft. The MTP and DFlash routes
both need the checkpoint, which is also a training base. The owner also approved the 4B base
for the separately evaluated 4B training experiment (see
[OFFLINE_TRAINING.md](OFFLINE_TRAINING.md)). All three are Apache-2.0 and ungated.

`Config/training_assets.json` pins every file's size and SHA-256 at each revision, and
`Tools/install_training_assets.py` downloads only those files. Each installed directory
gets a deterministic `revia-install-manifest.json`, so its digest is known before the
download starts:

| Asset | Repository at revision | Files | Bytes | Install manifest SHA-256 |
| --- | --- | --- | --- | --- |
| `qwen3.5-9b-base` | `Qwen/Qwen3.5-9B` at `c202236235762e1c871ad0ccb60c8ee5ba337b9a` | 15 | 19,329,392,091 | `84db5109…69be` |
| `qwen3.5-9b-dflash` | `z-lab/Qwen3.5-9B-DFlash` at `5fc3b3d474760f18c516db87d84c37edbfd3ede6` | 3 | 2,583,824,409 | `32ad85f5…bf85` |
| `qwen3.5-4b-base` | `Qwen/Qwen3.5-4B` at `851bf6e806efd8d0a36b00ddf55e13ccb7b8cd0a` | 13 | 9,342,905,899 | `f5a92a1b…96a5e` |

Install one at a time on the host. `--plan` prints the full digest and the destination
without downloading anything:

```powershell
python -I -B Tools\install_training_assets.py --asset qwen3.5-9b-base --destination D:\ReviaModels --plan
python -I -B Tools\install_training_assets.py --asset qwen3.5-9b-base --destination D:\ReviaModels
python -I -B Tools\install_training_assets.py --asset qwen3.5-9b-dflash --destination D:\ReviaModels
python -I -B Tools\install_training_assets.py --asset qwen3.5-4b-base --destination D:\ReviaModels
```

The installer refuses OneDrive, network and linked destinations. It never replaces an
existing file, resumes an interrupted file with a byte range, and discards any download
whose size or digest does not match. It writes the manifest only after every file
verifies, and a receipt under `_revia_download`. Checks on Linux: 7 fixture tests against a
local hub with redirects, range resume, dropped connections and corrupted bytes. Removing
the digest check or the range resume each made its test fail. One real installation of
two pinned files from the 4B revision on huggingface.co matched their pinned SHA-256.

Conversion uses the `b10453` converter with `--mtp` (MTP head) and `--target-model-dir`
(DFlash); the existing Main GGUF and projector stay as they are. Nothing is converted or
enabled yet.

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
