# Offline training data and jobs

Optional, and not part of normal startup. Memory changes what Revia retrieves, skills
change reusable procedures, and the self-development stages change verified code. This
path is the only one meant to change model parameters, and nothing in it runs by
itself.

Status on 2026-10-31:

- The dataset exporter, the training worker, the pinned asset installer, the llama.cpp
  conversion and check tool, and the evaluation runner exist.
- Their checks pass on Linux. The exporter and worker checks also passed in the Windows CI
  build.
- A tiny Qwen3.5 has been trained on CPU and taken through llama.cpp end to end.
- The owner approved the downloads, including the 4B base for the separately evaluated 4B
  experiment.
- No runtime capture, owner review screen, real export or real training job exists yet.

## Dataset export

`Learning/datasetExport` (`BuildDatasetExport`, `StaleDatasetExamples`,
`WriteDatasetExport`) works on `DatasetCandidate` records. Each record carries:

- the exact model input and the desired response;
- the quality receipt: judgment, criterion, judged output digest, and the profile and
  configuration fingerprints the runtime captures;
- the owner's review decision;
- the model, template and capture-transformation identities;
- every source ID the input used.

An example is eligible only when all of the following hold:

- Its review is **Accept**.
- Its receipt is valid and belongs to the same session.
- Its target is one of two things:
  - an owner correction whose rejected side is byte-for-byte the judged failure, and that
    does not repeat it; or
  - a model output whose exact digest a satisfied criterion recorded.
- None of its sources is withdrawn, including the judged turn itself.
- The shared secret gate finds nothing in it.
- It does not duplicate an earlier example.

Every exclusion is recorded in the manifest with its reason. A failed answer is never
written as an assistant message. It appears only as `rejected` beside its correction, for
any later preference training.

Grouping comes before splitting. Examples that share a session, a task, any source or an
identical prompt form one group, and a salted hash of the group places it in train,
validation or test. Given the previous manifest, an example keeps the partition it had.
If a new example would join groups that earlier exports kept apart, the group follows its
most protected member (test, then validation), and any member that would otherwise move
is excluded instead. So nothing a model trained on can later appear as a held-out test
case. The same candidates, withdrawals, options and previous manifest produce the same
bytes in any input order. `manifest.json` records each file's SHA-256 and every decision.

`WriteDatasetExport` writes a new directory through a `.partial` staging directory, and
never replaces an export or ignores an unfinished one. Immediately before a worker reads
an export, `StaleDatasetExamples` compares the manifest's per-example sources with the
current withdrawals. Any result means that export must not be used; prepare a new one. A
completed weight update cannot be undone by deleting an example. The candidate trained on
it is retired or retrained instead (see the promotion workflow, when it exists).

Evidence: `Tests/datasetExportTests.cpp` passed on Linux against the production owner.
Removing the review gate, the earlier-partition rule, source grouping or the canonical
ordering each made it fail. It is registered as `ReviaDatasetExportTests`.

Still needed before a real export:

1. The runtime must capture the exact serialized provider request from the correlated
   Stage E turn trace. A transcript reconstruction is not the model's input.
2. A source list must be complete for derived data, such as frame extracts and captions.
3. An owner review step must accept a correction.
4. The withdrawal set must come from the attachment library's and memory's own Forget
   receipts.

## Training worker

`Tools/revia_training_worker.py` runs one supervised BF16 LoRA job on a dataset export.
It uses the same contract as the video worker:

- The job is `ReviaTrainingJobs/<jobId>/job.json` in an owned scratch root, separate from
  the candidate output root.
- Progress is one JSON line per state.
- Creating `cancel` in the job directory stops the job at its next step, after saving a
  checkpoint.
- Each attempt leaves its own `result.json` (earlier attempts are kept as
  `result-N.json`).
- Exit codes: 0 complete, 1 failed, 3 cancelled.

The worker imports torch, transformers and peft only when a job asks for the
`transformers-peft` backend. A missing runtime is refused by name, so normal startup never
depends on them.

Before any model work, the worker checks the base checkpoint against its installed
`revia-install-manifest.json`. That is a JSON object with `repository`, `revision` and
`files` (each file's `bytes` and `sha256`), written by the approved download step. The
check covers its digest, repository and revision, every file's size, and a fresh hash of
each file of 64 MiB or less, such as the config, tokenizer and chat template. It checks
the dataset manifest and the train and validation files against their digests. It reruns
the withdrawal check against the current list the host writes beside the job. The test
partition is never opened.

Configurations other than BF16 supervised LoRA are refused. Quantized LoRA is refused
with the reason the current Qwen3.5 guidance gives: 4-bit training shows larger than
normal quantization differences. A checkpoint from another base, dataset or configuration
is refused rather than resumed. An existing candidate is never replaced.

Examples are rendered with `enable_thinking=False`, the setting Revia uses for ordinary
turns, and only the assistant reply, including its end-of-turn token, is supervised. An
actual Qwen3.5 template check found that with thinking on, the generation prompt
(`<think>\n`) is not a token prefix of a rendered reply (`<think>\n\n</think>\n\n…`), so
naive response masking would be wrong. The worker therefore refuses any example whose
prompt is not an exact token prefix. Reasoning-mode examples are not supported yet. The
same guidance recommends keeping at least 75% reasoning examples to preserve reasoning, so
an adapter trained only on direct replies must be evaluated for reasoning retention before
any promotion.

A finished job publishes `adapter-<jobId>/` through a staging directory. Its
`candidate.json` records:

- the base identity, including the template digest and model shape;
- the dataset identity, including the withdrawal list digest;
- the exact configuration and its digest;
- the rendering and masking rule;
- train and validation loss, and the environment versions;
- every file's digest.

Its state is `candidate`. Nothing activates, converts or merges it.

Evidence, all on Linux:

- `Tests/reviaTrainingWorker.test.py` covers orchestration with a deterministic fixture
  backend, in 11 tests. Removing the withdrawal recheck, reading the test partition or
  dropping the checkpoint identity check each made its own test fail.
- With `REVIA_TRAINING_SMOKE_MODEL` set, the real backend trained
  `trl-internal-testing/tiny-Qwen3_5ForConditionalGeneration-Think` (`6860d7ec`, a hybrid
  linear-attention plus full-attention Qwen3.5) on CPU. The software was torch 2.15.0+cpu,
  transformers 5.22.1 and peft 0.21.2. The run was cancelled at its step-2 checkpoint,
  resumed and finished, and the published adapter changed the base model's output.
- Without optimized kernels, transformers fell back to reference implementations of the
  gated delta rule and causal convolution. A real job's environment should include
  `flash-linear-attention` and `causal-conv1d`.

This is fixture and tiny-model evidence: no GPU, real base or real dataset has been used.

Run the checks, and the worker once prerequisites exist:

```powershell
python Tests\reviaTrainingWorker.test.py
```

```powershell
<training-python> -I -B Tools\revia_training_worker.py --job <scratch>\ReviaTrainingJobs\<jobId>\job.json
```

## Conversion and llama.cpp check

`Tools/revia_adapter_export.py` takes a candidate from a verified base to llama.cpp files.
It reuses the worker's base verification, and each command writes a new directory with an
`export.json` receipt.

- `base` converts the verified Hugging Face base to BF16 GGUF with the pinned llama.cpp
  converter and quantizes it (Q4_K_M by default). This is the matched baseline. The official
  Qwen3.5 checkpoints carry their MTP head, which the conversion keeps (`mtpHead`), so the
  baseline can also be used for MTP speculation. A config that declares a head its weights
  lack gets `--no-mtp`, because converting it would write a block the loader cannot find.
- `adapter` converts a candidate to a GGUF LoRA with `convert_lora_to_gguf.py`. It first
  refuses a candidate whose recorded base is not exactly this base, so a 4B adapter can never
  be converted for the 9B. The quantized base never changes, and serving without `--lora`
  rolls the adapter back.
- `check` runs llama-server on the baseline, and again with the adapter. Each run is
  compared with the Hugging Face base on:
  - the embedded chat template;
  - the end-of-turn token;
  - chat rendering with thinking off, with and without tool definitions;
  - tokenization of the rendered prompt;
  - the GGUF block count, including any MTP block.

  Greedy replies from the two runs are recorded for comparison but are not a gate.

The converter runs under its own interpreter (`--converter-python`), an environment built
from the tag's `requirements/requirements-convert_hf_to_gguf.txt`. That file pins
transformers 4.57.6, while training needs transformers 5. The pinned converter cannot load a
tokenizer saved by transformers 5 (`TokenizersBackend`). The official Qwen3.5 checkpoints name
`Qwen2Tokenizer`, which it loads.

Evidence, all on Linux:

- `Tests/reviaAdapterExport.test.py` covers the refusals in 6 fixture tests.
- A full roundtrip on the tiny Qwen3.5 passed every gate on both runs. It trained an adapter
  on attention plus MLP, converted the base with llama.cpp `b10453` (commit `3cb7ffb1a1f6`)
  using the converter environment (torch 2.11.0, transformers 4.57.6, numpy 1.26.4), and
  converted the adapter. Where both runs produced text, the adapter's replies differed from
  the base's.
- The tiny model's 16-wide tensors cannot be block-quantized, so its smoke run requantizes to
  F16. The real bases are 2,560 and 4,096 wide and use Q4_K_M.
- llama-server `b10453` parses every generation against the template's output format, and
  rejected some of the random model's text. That is why replies are informational.

## 4B experiment (chosen 2026-10-31)

The 4B adapter is a separately evaluated experiment on the official `Qwen/Qwen3.5-4B`. It is
never attached to the 9B Main. Using any 4B model in Revia would be a separate
model-selection decision. Steps on the host:

1. Install the base outside OneDrive:
   `python -I -B Tools\install_training_assets.py --asset qwen3.5-4b-base --destination D:\ReviaModels`.
   It lands in `D:\ReviaModels\Qwen3.5-4B-851bf6e`, with install manifest SHA-256
   `f5a92a1bf46485d70bd38e9643ee6dd29bb674029733fe0fddf097db45396a5e`.
2. Build two Python 3.12 environments.
   - Training: a CUDA build of torch for the RTX 5070, transformers 5, peft, safetensors,
     flash-linear-attention and causal-conv1d.
   - Converter: from the llama.cpp `b10453` checkout's converter requirements.

   Then run the three test files with the smoke variables set:

   ```powershell
   $env:REVIA_TRAINING_SMOKE_MODEL = "<tiny model dir>"
   $env:REVIA_LLAMA_CPP_DIR = "<b10453 checkout>"
   $env:REVIA_LLAMA_BIN_DIR = "ThirdParty\llama.cpp"
   $env:REVIA_CONVERTER_PYTHON = "<converter venv>\Scripts\python.exe"
   ```

   The tiny model is `trl-internal-testing/tiny-Qwen3_5ForConditionalGeneration-Think` at
   `6860d7ec`. This proves the host environments before the 9 GB job.
3. Produce a reviewed dataset export, and write the current withdrawal list as
   `withdrawn.json` beside the job just before starting it. **This is the remaining blocker.**
4. With Revia stopped, run the job below. The 10,500 MiB free-VRAM floor enforces that, since
   Main and the voice worker would not leave that much free. Run it first with `maxSteps` 5:
   `candidate.json` then records `peakVramMiB`. The guidance's 10 GB figure assumes Unsloth's
   optimizations, so plain peft on a 12 GB card has to be measured, and `maxSequenceTokens`
   lowered if needed.
5. Export the matched baseline and the adapter, then run `check` with
   `--server-arg=-ngl --server-arg=99`.
6. Evaluate with the runner described below. Plan the campaign first, then run the base arm and
   the candidate arm, write the review sheet, judge it, and score it. The criteria are fixed
   when the plan is written:
   - the held-out test partition improves, with no new failures;
   - no more than one regression across the golden set, the answer-quality holdout and the
     tool cases;
   - prefill and decode rates within 5% of base;
   - the `check` passed.

   The base arm is the same file served without `--lora`, so it is also the rollback.
   A rejected candidate is a valid result.

```json
{
  "schema": "revia-training-job-v1",
  "jobId": "qwen35-4b-companion-1",
  "scratchRoot": "D:/ReviaTraining/ReviaTrainingJobs",
  "outputRoot": "D:/ReviaTraining/candidates",
  "withdrawnSourcesPath": "D:/ReviaTraining/ReviaTrainingJobs/qwen35-4b-companion-1/withdrawn.json",
  "base": {"directory": "D:/ReviaModels/Qwen3.5-4B-851bf6e", "repository": "Qwen/Qwen3.5-4B",
           "revision": "851bf6e806efd8d0a36b00ddf55e13ccb7b8cd0a",
           "installManifestSha256": "f5a92a1bf46485d70bd38e9643ee6dd29bb674029733fe0fddf097db45396a5e"},
  "dataset": {"directory": "<reviewed export directory>", "manifestSha256": "<SHA-256 of its manifest.json>"},
  "resources": {"device": "cuda:0", "minimumFreeVramMiB": 10500, "minimumFreeRamMiB": 16384, "minimumFreeDiskMiB": 20480},
  "configuration": {"method": "sft-lora", "backend": "transformers-peft", "precision": "bf16", "enableThinking": false,
                    "seed": 20261031, "maxSteps": 5, "learningRate": 0.0001, "gradientAccumulation": 8,
                    "maxSequenceTokens": 4096, "loraRank": 16, "loraAlpha": 32, "loraDropout": 0.05,
                    "targetModules": ["q_proj", "k_proj", "v_proj", "o_proj", "gate_proj", "up_proj", "down_proj"],
                    "checkpointEverySteps": 5}
}
```

Qwen3.5 projections differ by layer type. Gated DeltaNet layers use
`linear_attn.in_proj_qkv/z/b/a` and `out_proj`, the full-attention layers (every fourth) use
`self_attn.q/k/v/o_proj`, and every layer has `mlp.gate/up/down_proj`. The listed targets
therefore cover attention where it exists and MLP everywhere. The Gated DeltaNet projections
stay frozen until measured. After the short run, set `maxSteps` to about three epochs,
`ceil(3 × train examples / 8)`, use a new `jobId`, and keep the configuration otherwise
fixed.

## Evaluation runner

`Tools/revia_adapter_evaluation.py` compares a candidate with its matched base. It judges
nothing itself: mechanical checks stay with the existing evaluation owners, and the
held-out replies go to the owner.

- `plan` writes `plan.json` into a new campaign directory before anything is generated. It
  records digests of:
  - the base and adapter exports and their passed `check`;
  - the dataset's held-out partition;
  - the golden set, the answer-quality holdout and `Config/Evaluation/adapter-tool-cases.json`;
  - the llama-server, `ReviaAnswerGateLive` and `ReviaAnswerQualityLive` binaries and the
    profile;
  - the sampling settings: temperature 0, a seed, thinking off.

  It also records the criteria. The plan is refused if:
  - the candidate was trained on a different dataset export;
  - the check did not pass for exactly this base and adapter;
  - the held-out partition is smaller than the declared minimum (5 by default).

  Every later command rechecks all of those digests, so nothing can change between fixing the
  criteria and applying them.
- `run --arm base|candidate` serves one arm from an owned llama-server with the arm's alias: the
  quantized base, then the same file with `--lora`. It then:
  - replays each held-out prompt exactly as captured, without its reviewed reply;
  - calls the tool cases and judges each call mechanically (right function and arguments, and
    no call when none is needed);
  - runs `ReviaAnswerGateLive` on the golden set with `--expected-model` set to the arm's alias
    and `--ignore-baseline`, without a judge unless the plan names a judge port;
  - runs `ReviaAnswerQualityLive` on the holdout.

  Each arm runs once into its own directory and is never rerun in place.
- `review` writes `review.json`. Each held-out case shows its criterion, conversation and
  reviewed reference, with the two replies as A and B in a seeded random order. The mapping
  goes to `review-key.json`, which the reviewer should not open. A blank
  `verdicts.template.json` is written alongside.
- `score --verdicts <file>` writes `decision.json`: accept, reject, or incomplete while evidence
  is missing. Any failed criterion rejects. Retention regressions are cases the base passed and
  the candidate fails. Latency compares the medians of llama-server's own `prompt_ms` and
  `predicted_per_second`, over only the requests both arms completed. Golden-set judge scores,
  when present, are reported as advisory. Nothing is activated.

Evidence, all on Linux:

- `Tests/reviaAdapterEvaluation.test.py` passed its 7 fixture tests. They use a loopback model,
  and stand-ins that reproduce the quality tools' arguments, result files and exit codes.
- Each of these removals made a test fail: the no-new-failures rule, matched latency, and the
  plan's digest recheck.
- A smoke run planned both arms against a real llama-server `b10453` on the tiny Qwen3.5, then
  ran, reviewed and scored them. Without verdicts it could not accept. It rejected on latency,
  which is correct, because LoRA overhead on a tiny CPU model is large.
- The smoke run also found a bug in `check`: run results overwrote the base identity and the
  adapter flag. Run results now sit under `runs`, and the roundtrip test asserts the identity.

On the host, with Revia stopped so both arms get the 5070:

```powershell
python -I -B Tools\revia_adapter_evaluation.py plan --output D:\ReviaTraining\evaluations\qwen35-4b-companion-1 `
  --base-export D:\ReviaTraining\exports\qwen35-4b-base --adapter-export D:\ReviaTraining\exports\qwen35-4b-companion-1 `
  --check D:\ReviaTraining\exports\qwen35-4b-companion-1-check.json --dataset <reviewed export> `
  --server ThirdParty\llama.cpp\llama-server.exe --gate <build>\ReviaAnswerGateLive.exe `
  --quality <build>\ReviaAnswerQualityLive.exe --profile Config\Profiles\revia.json --server-arg=-ngl --server-arg=99
python -I -B Tools\revia_adapter_evaluation.py run --plan D:\ReviaTraining\evaluations\qwen35-4b-companion-1 --arm base
python -I -B Tools\revia_adapter_evaluation.py run --plan D:\ReviaTraining\evaluations\qwen35-4b-companion-1 --arm candidate
python -I -B Tools\revia_adapter_evaluation.py review --plan D:\ReviaTraining\evaluations\qwen35-4b-companion-1
python -I -B Tools\revia_adapter_evaluation.py score --plan D:\ReviaTraining\evaluations\qwen35-4b-companion-1 --verdicts verdicts.json
```

The `score` exit code is 0 for accept, 1 for reject and 2 for incomplete.

## Remaining blockers

- **Reviewed data.** No reviewed export exists yet. The capture steps under Dataset export
  come first. The Stage E failures in `build/stage-e-20261030` retain their exact serialized
  requests and raw responses, which makes them the natural first examples once the owner
  writes corrections.
- **Evaluation runner on the host.** The runner exists, but on this branch it has only driven
  stand-ins for the C++ quality tools. Its first host run is also the first check that
  `ReviaAnswerGateLive` and `ReviaAnswerQualityLive` work against an owned llama-server.
- **A 9B adapter.** Training it locally is still infeasible as configured: about 22 GB for
  BF16 LoRA against the 12 GB 5070.
