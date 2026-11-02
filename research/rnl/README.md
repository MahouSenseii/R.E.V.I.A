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
- FIRST-001 ran reviewed CPU toy code in owned workers. No dependencies, GPU,
  training, model download, or other running workload was used.

## Status ledger - 2026-11-02

| Stage | Meaning | Current evidence |
| --- | --- | --- |
| Experimental | Hypothesis, static review or proposed experiment | Packed-runtime and matrix-cache instrumentation; no language-model claim |
| Measured | Reproducible result with pinned inputs, method and limits | FIRST-001 storage/equivalence, RNL-002 exact-cache contract and RNL-003 conditional synthetic CPU benefit |
| Candidate for Revia | Independently reviewed benefit plus relevant held-out checks | None |
| Integrated | Explicitly authorized, reviewed change landed in Revia with rollback | None |

These stages are separate decisions. A completed run, smaller matrix payload or
passing toy test does not promote an idea automatically.

RNL-002 then tested one correction to the failed cache contract: reuse only the
raw matrix product, keyed by selected expert and all input bits, while always
rerouting and updating state. M matched the uncached reference bit-for-bit over
280 steps, with 77 hits. Omitting the expert from the key failed its planned
control. This is numeric equivalence on a pinned toy, not a speed or RAM result.
See the [RNL-002 report](RESULTS-RNL-002.md), [pre-registered protocol](RNL-002.md),
[status](RNL-002-status.json) and [evidence](results/RNL-002/summary.json).
RNL-003 then measured the complete event at synthetic square sizes 16, 64, 256
and 1024 with fixed 0%, 50% and 90% repeated-event controls. All 2,400 validation
steps matched bit-for-bit. Eight repeat conditions passed the predeclared local
benefit rule (median paired U/M 1.213-3.994); all four zero-hit controls had no
benefit. M added retained buffers and demonstrated no RAM saving. These are
generated networks, not Revia/model traffic or language quality. See the
[RNL-003 report](RESULTS-RNL-003.md), [protocol](RNL-003.md),
[status](RNL-003-status.json) and [evidence](results/RNL-003/summary.json).
This one study is concluded. No further model, training or integration work runs
automatically; relevance to real Revia inputs is a later decision.

The reviewed [applicability audit](APPLICABILITY-001.md) preserves that positive
finding while identifying no current demonstrated Revia matrix-cache target.
Existing prefix reuse and exact-input embedding caching already cover relevant
work. Two source-supported leads remain: qualify existing prefix reuse, and
establish embedding-cache lifecycle coverage. Observations, hypotheses, smallest
discriminators and unknown installed-runtime identity are recorded separately.
No proposed fixture or model experiment has run; the next authorized activity
is only a read-only embedding lifecycle investigation after documentation publication.

The owner supplied the original local ZIP. After safe extraction, source review
and bounded execution, F/U/P matched bit-for-bit over 236 steps each. The authored
cache changed recurrent behavior. Complete model files measured 200/131/113 bytes;
neither faster execution nor lower process memory was demonstrated. See the
[results and limits](RESULTS-FIRST-001.md), [evidence](results/FIRST-001/summary.json)
and [current status](FIRST-001-status.json). The earlier blocked transfer and null
results remain in commit `2f2b550`; the official transfer helper was not patched.

## Static baseline and provenance

The original archive is 15,706 bytes, SHA-256
`a750f92354510db6337bb57d589d010ccc8caf8d5ec76aee407984e069678208`.
Its seven entries were path-checked, hashed and inspected before execution.
Original bytes remain unchanged outside the repository; source is not vendored.
The earlier static findings below are now confirmed against actual source. Their
runtime limits and measured counterexamples are recorded separately in the report.

- The prototype is a handcrafted 3-input, 3-state, 3-expert top-1 codebook network.
  It has no tokenizer, training, text inference or GGUF support, and does not run
  Revia's Qwen model.
- Its 9-byte packed matrix payload expands to 27 byte-sized indices on load, with
  all experts resident. Codebooks, routing, state, metadata and allocation overhead
  also count. Matrix disk size is not process memory or VRAM.
- The HTML reports 3 multiply-accumulates while its matrix loop performs 9. The
  C++ source implements zero-input skipping; router, activation and state costs remain
  additional work. These counters are not a measured speedup.
- The repeated-input cache freezes state even though routing and output depend on
  state. Near-repeat skipping has no established error bound. Both require
  trajectory checks, not just a final output comparison.
- The C++ source uses the inspect-size expression
  `E*(N+3)/4` instead of per-expert packing `E*((N+3)/4)`.
- A full 113-byte binary compared with a 108-byte FP32 matrix is disclosed in the
  prototype; those quantities have different boundaries. Existing tests do not
  establish numeric state equivalence, language quality or VRAM savings.

The original CLI always caches, starts state at zero and prints three decimals.
It supplies smoke evidence, not full-precision recurrence proof. Separately
reviewed instrumentation and an independent FP32 oracle provide that experiment's
traces. The original source was not changed to correct its findings.

## FIRST-001 - CPU equivalence and honest accounting

**Question:** Can packed indices preserve this toy network's full computation,
and what does each representation actually cost?

The bounded experiment has run, with the limits recorded in its report. The
protocol compares three representations of the **same
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

Declared budgets: at most one CPU compute thread, a 256 MiB working
RSS cap, at most 10 minutes execution, and less than 10 MiB of result artifacts.
Zero GPU use, training or model downloads. These caps may yield an incomplete run.
Keep a small evidence record with source/input digests, environment, command,
settings, timestamps, exit status, trajectories and accounting/timing summaries;
reuse existing catalog/evidence conventions instead of building a new framework.

## Downstream comparison and promotion

FIRST-001 provides toy equivalence; RNL-003 provides conditional synthetic runtime
benefit. No candidate is promoted. A separate proposal would need a relevant
measured benefit on Revia's actual computation before comparing a change
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
