# RNL research sandbox

RNL is a separate, bounded investigation into whether codebook compression or
expert routing can help Revia. It is not a replacement language model, a training
system, or a prerequisite for improvements to Revia's memory and corrections.
Useful results may become small, separately reviewed Revia changes later.

## Scope and isolation

- Research branch: `research/rnl-sandbox`, started from verified GitHub `main`
  commit `281514d3be4e651ae9cfd18219a41e99d08e652c`.
- Keep experiments and their evidence under `research/rnl/`; do not alter the
  ongoing reliability/delegation work, application settings, or running services.
- The public research snapshot has one content source:
  [`docs/atlas/research.html`](https://mahousenseii.github.io/R.E.V.I.A/research.html). Future research
  updates change only that page. Existing Atlas milestone claims, totals and
  `milestone.json` are outside this workstream. Initial navigation and publication
  wiring are a separately scoped documentation change.
- No uploaded source has been executed, no dependencies installed, and no GPU,
  training, model download, or running workload used for this setup.

## Status ledger - 2026-11-02

| Stage | Meaning | Current evidence |
| --- | --- | --- |
| Experimental | Hypothesis, static review or proposed experiment | FIRST-001 setup checked; blocked at original-input gate |
| Measured | Reproducible result with pinned inputs, method and limits | None |
| Candidate for Revia | Independently reviewed benefit plus relevant held-out checks | None |
| Integrated | Explicitly authorized, reviewed change landed in Revia with rollback | None |

These stages are separate decisions. A completed run, smaller matrix payload or
passing toy test does not promote an idea automatically.

The execution request has reached the input gate. Refreshed Library preparation
still requires a download; it does not supply local bytes. The current official
helper is unchanged from the failed transfer, Windows Python lacks its required
extended-attribute operation, and no existing WSL runtime is available. No helper
was patched and the known-identical failure was not rerun. No benchmark has run.
The [FIRST-001 protocol](FIRST-001.md) and [setup status](FIRST-001-status.json)
record the next steps and preserve unmeasured fields as null. An owner-provided
original ZIP at an explicitly confirmed local path, or a supported Library
materialization runtime, is needed before source review and execution.

## Static baseline and provenance

The supplied HTML illustration is readable through Library. Local attachment
materialization is blocked by the transfer helper's unsupported Windows extended
attribute operation; the ZIP is not available as readable source in this checkout.
The C++ findings below are **inherited from the earlier static audit**, not
independently re-verified here. None are runtime measurements.

- The prototype is a handcrafted 3-input, 3-state, 3-expert top-1 codebook network.
  It has no tokenizer, training, text inference or GGUF support, and does not run
  Revia's Qwen model.
- Its 9-byte packed matrix payload expands to 27 byte-sized indices on load, with
  all experts resident. Codebooks, routing, state, metadata and allocation overhead
  also count. Matrix disk size is not process memory or VRAM.
- The HTML reports 3 multiply-accumulates while its matrix loop performs 9. The
  C++ audit reports zero-input skipping; router, activation and state costs remain
  additional work. These counters are not a measured speedup.
- The repeated-input cache freezes state even though routing and output depend on
  state. Near-repeat skipping has no established error bound. Both require
  trajectory checks, not just a final output comparison.
- The inherited C++ audit found the inspect-size expression
  `E*(N+3)/4` instead of per-expert packing `E*((N+3)/4)`.
- A full 113-byte binary compared with a 108-byte FP32 matrix is disclosed in the
  prototype; those quantities have different boundaries. Existing tests do not
  establish numeric state equivalence, language quality or VRAM savings.

First recover and statically review the exact ZIP bytes through a supported
transfer route. Record input SHA-256 digests and tool versions before proposing
execution. Unavailable inputs stay unavailable; do not reconstruct them from this
summary or quietly substitute another prototype.

## FIRST-001 - CPU equivalence and honest accounting

**Question:** Can packed indices preserve this toy network's full computation,
and what does each representation actually cost?

Proposed only; no benchmark harness or uploaded program has run. After input
recovery and execution review, compare three representations of the **same
decoded weights**: packed codebook indices, unpacked uint8 indices and FP32
weights. Keep expert selection, routing, initial state, activation, accumulation
order and inputs fixed. Disable state-skipping caches for the reference comparison.

Use deterministic reset-and-run sequences covering zeros, nonzero inputs,
repeated inputs, near-repeats on both sides of the cache threshold, and changes
that exercise expert selection. Record every selected expert, output and state
component at every step. Compare cache-enabled repeats and near-repeats separately
against that reference as negative controls; disclose divergence rather than
counting skipped work as an equivalent optimization.

Report separately:

- Matrix payload, codebooks, padding, metadata and complete serialized model size;
  keep executable size separate and use the same model boundary for FP32.
- Runtime representation bytes, all resident experts, scratch/state and peak
  process RSS, with the empty-process baseline and measurement method stated.
- Counted matrix operations and routing/state/activation work; label analytic
  counts separately from timed measurements.
- End-to-end load latency and per-step latency, including unpacking/routing/cache
  costs. Use a warm-up and at least five identical timed batches; report median,
  range, timer resolution and workload size. If overhead or noise dominates this
  tiny model, mark latency inconclusive.

**Success:** all representation variants choose identical experts and match every
state/output component within a predeclared absolute tolerance of `1e-6`, all
storage/runtime boundaries are reconciled, and repeat controls expose any cache
semantic change. Any savings must survive those same boundaries; equivalence
alone is not a performance improvement or evidence of language capability.

**Failure:** an expert differs, any trajectory exceeds tolerance, accounting
cannot be reconciled, or a claimed equivalent cache changes state. Preserve that
result and its counterexample; do not relax criteria after seeing results.

**Stop:** missing/unreviewed input, unsupported measurement, cap exceeded, required
dependency/download, unexpected service access or interference with other work.
Stop rather than expand scope. Record failures, interruptions and unmeasured fields.

Planning budgets, **not measured usage**: at most one CPU thread, a 256 MiB working
RSS cap, at most 10 minutes execution, and less than 10 MiB of result artifacts.
Zero GPU use, training or downloads. These caps may yield an incomplete run.
Keep a small evidence record with source/input digests, environment, command,
settings, timestamps, exit status, trajectories and accounting/timing summaries;
reuse existing catalog/evidence conventions instead of building a new framework.

## Downstream comparison and promotion

Only after FIRST-001 passes should a separate proposal compare a relevant change
against a pinned Qwen/llama.cpp baseline with identical model, prompts, context,
sampling, hardware and measurement boundaries. The toy is not a text-model
baseline. Model RAM/VRAM, runtime and evaluation costs remain unknown until those
identities and settings are pinned; no Qwen or GPU run is authorized by this plan.

Reuse [the answer-quality gate](../../docs/EVALUATION_GATE.md) and
[the existing golden set](../../Config/Evaluation/golden-set.jsonl) where relevant;
keep held-out quality, safety and privacy checks independent of tuning. Add
task-specific checks only when the change requires them, and retain incomplete
and adverse outcomes. Do not infer release readiness from a toy result.

Any future training follows [the offline-training workflow](../../docs/OFFLINE_TRAINING.md):
consented examples, complete source lineage, owner review, withdrawal checks and
protected train/validation/test splits. Deleting a source does not erase its
influence from trained weights; affected candidates may need retirement or
retraining. No autonomous self-training is enabled here.

Promotion needs independent review, a measured useful benefit, held-out
quality/safety/privacy evidence, explicit authorization, and a versioned rollback
to the unchanged baseline. Research does not merge into Revia automatically.

Background, not validation of this prototype:
[AQLM](https://arxiv.org/abs/2401.06118),
[Switch Transformers](https://arxiv.org/abs/2101.03961), and
[QLoRA](https://arxiv.org/abs/2305.14314).
