# Offline training data and jobs

Optional, and not part of normal startup. Memory changes what Revia retrieves, skills
change reusable procedures, and the self-development stages change verified code. This
path is the only one meant to change model parameters, and nothing in it runs by
itself. Status on 2026-10-31: the dataset exporter exists and passes fixture checks on
Linux. The training worker
exists and has trained a tiny model on CPU. No runtime capture, owner review screen, real
export or real training job exists yet.

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

## Blockers for a real Main adapter

- **Hardware.** The guidance's BF16 LoRA figure for 9B is about 22 GB. Main's GPU has
  12 GB, the RTX 2070 SUPER has 8 GB and no native BF16, and the two are not a pooled
  allocation. A 9B adapter cannot be trained locally as configured. Each alternative is
  a decision for the owner:
  - a separately evaluated 4B candidate, at about 10 GB, which fits the 5070 only with
    Main and the voice worker unloaded. It is never attachable to the 9B Main.
  - a measured CPU-offload configuration, untested and likely very slow.
  - a rented GPU, which is not authorized.
- **Base checkpoint.** The official `Qwen/Qwen3.5-9B` checkpoint is a prepared, not yet
  approved, 19.3 GB download, shared with the speculation work in
  [SPECULATIVE_DECODING.md](SPECULATIVE_DECODING.md).
- **Reviewed data.** No reviewed dataset exists yet. The runtime capture steps listed
  under Dataset export come first.
- **Conversion roundtrip.** Not implemented: adapter, then evaluation, merge or export,
  GGUF, and llama.cpp evaluation. Template and end-of-turn behavior must be checked before
  and after quantization.
