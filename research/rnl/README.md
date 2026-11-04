# RNL research sandbox

RNL is a separate, bounded investigation into compression, exact reuse and tiny
trainable state, with an adaptive/stateful architecture as a broader research
goal. It does not replace or train Revia's installed language model. Practical
memory and corrections remain a separate path. Useful results may become small,
separately reviewed Revia changes later.

## Latest measured study - STATE-010

The nonlinear reader showed no consistent benefit. Direct/identity/ReLU readers
answered 27.88%/29.56%/27.93% of individual questions correctly, versus 12.5%
references, but every arm/seed scored **0/168 complete four-case groups** and
missed all fixed screens. Learning curves improved; this does not prove
convergence, absent facts or that broader RNL research is impossible.

Nine reader fits reused three saved encoders with zero encoder updates. The
single CPU run took 216.566 seconds; controls, 12096 retained predictions, 15 safe
FP32 snapshots and owned cleanup reconciled. Main matrix MACs and similar
parameters do not match total compute; supplied key selection is not routing
learned by the model. No LLM or Revia improvement is established.

Saved-row errors support investigating learned keyed write/read structure,
not another automatic activation grid. A design-only comparison would use a
fresh GRU on the same recipe and a simple external dictionary, which already
solves this symbolic task without training. No next model has run.

See [results](RESULTS-STATE-010.md), [method](STATE-010.md),
[status](STATE-010-status.json), [exact aggregates](results/STATE-010/summary.json),
[configuration](STATE-010-config.json), [worker](state010.py), [data](state010_data.py),
[worker controls](test_state010.py) and [data controls](test_state010_data.py).

## Earlier frozen-state readout - STATE-009

Trained-state readers answered 27.28% of individual questions correctly, versus
17.11% for trained readers over initial encoder states. A separately trained
query-blind mean reader scored 27.53%. Every arm/seed scored **0/168 complete
four-case groups**, missing all predeclared screens. Similar mean accuracy does
not establish equivalence or show that the query had no effect: the readout
policies differ in effective expressivity and training gradients.

Partial recall remains useful evidence: trained-state selected readers solved
both queries on 39/1,008 pairs, versus 24 for initial-state readers and zero for
blind by construction. A post hoc audit found 15 versus 3 three-of-four group-runs;
these share groups across seeds and are not a replacement success gate. Reusing
the other recorded key's output gives 533 versus 550 correct for trained selected,
with mixed seed signs. This fixed-sample key-corruption check adds no model call.

The single approved run took 305.576 seconds. All 24 original hashes, 12,096
predictions, 15 safe FP32 snapshots, resource caps and cleanup reconciled. Weights
remain local so future work can avoid encoder replay. No installed Revia model,
application integration or general language-quality result is established.

That nonlinear-reader follow-up is now completed in STATE-010 above. It does not
identify the cause of these earlier findings, which remain unchanged. Learned
keyed state and an end-to-end training-recipe control remain unrun hypotheses.

See [results and interpretations](RESULTS-STATE-009.md), [method](STATE-009.md),
[status](STATE-009-status.json), [exact aggregates](results/STATE-009/summary.json),
[executed configuration](STATE-009-config.json), [worker](state009.py),
[data and accounting](state009_data.py), [worker controls](test_state009.py) and
[data controls](test_state009_data.py).

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

## Status ledger - 2026-11-04

| Stage | Meaning | Current evidence |
| --- | --- | --- |
| Experimental | Hypothesis, static review or proposed experiment | Learned keyed write/read state versus a fresh GRU and exact dictionary is design only; no next model or training has run |
| Measured | Result with pinned inputs, method and limits; negative results count | FIRST-001 equivalence, RNL-002 exact-cache contract, RNL-003 conditional synthetic CPU benefit, one PREFIX-004 real-model reuse pair, STATE-006's six-cell no-go, STATE-007's modest learning gains/recency audit, STATE-008's partial individual answers/zero full groups, STATE-009's partial readout signal, and STATE-010's negative nonlinear-reader comparison; all full-group screens missed |
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
No model experiment ran during that audit. The subsequent read-only embedding
lifecycle check did not establish a supported changed-weight cache bug.

PREFIX-004 now preserves three separately approved CPU-only attempts on the
same Qwen3.5-0.8B artifact and public fixtures. Pair03 completed one shared-prefix
cache-off/cache-on pair:438 prompt tokens each,0/438 reused/evaluated with cache
off and399/39 with cache on. Both26-token answers matched the expected7200/D2 JSON.
This qualifies existing backend reuse on the fixture. It is one fixed-order pair,
not a reliable speedup, language-quality or usual4B-model result. Each fresh
primer cost about50.5s; target first-content times were51.408s/4.666s and complete
request times56.006s/9.211s. No RAM saving or new RNL architecture is established.

Attempt01's readiness stop and attempt02's30-second primer deadline remain
separately labeled, with unchanged raw hashes and public JSON objects. Pair03
changed HTTP to75s and reduced sampling to two fresh servers; all other caps and
inputs stayed pinned. It took172.890s including staging/cleanup, used no GPU,
and left no owned process or credential. See the [report](RESULTS-PREFIX-004.md),
[status](PREFIX-004-status.json), [three-attempt evidence](results/PREFIX-004/summary.json),
[original full-study protocol](PREFIX-004.md) and [identity](PREFIX-004-identity.json).

STATE-006 completed its tiny standard GRU/RNN SET/QUERY comparison: six cells,
three seeds and1,536 updates in one447.698s operationally successful batch.
The scientific result is **no-go for this fixed task and budget**. GRU long-delay
accuracy was12.11%,12.89%,16.21%, versus RNN10.16%,11.91%,14.26%; uniform chance
and a key-only predictor are12.5%. Gains of0.98-1.95 percentage points missed
the original90% accuracy/10-point advantage requirements. GRU overwrite-pair
success was0/128,1/128,3/128; this is a two-answer metric, not per-query accuracy.

No loss curve was retained. The two held-out A checkpoints record above-baseline short-lag
accuracy and a decline after the longer-lag phase; without initial accuracy,
training-induced improvement is unmeasured. They cannot establish
convergence or identify task difficulty, exposure budget, optimization or
architecture as the cause. State reset every64-event episode means this was
within-episode memory, not persistence across requests or a custom RNL design.
The original setup failure and intermediate timing/admission failures remain
historical evidence; a separately approved single-deadline continuation completed.
Candidate for Revia and Integrated remain None. No further run follows automatically.

The earlier A-only learning-exposure proposal was revised into STATE-007 to hold
the unique examples fixed while
adding repeated updates. The STATE-006 [report](RESULTS-STATE-006.md),
[status](STATE-006-status.json), [evidence](results/STATE-006/summary.json) and
historical protocol remain unchanged. Its longer-delay no-go was not repaired
or superseded by the narrower follow-up.

STATE-007 completed six cells / three paired seeds, repeating each original
3,072-example short-delay A pool for1,152 updates. Mean diagnostic accuracy rose
from30.60% to32.94% for GRU and22.66% to30.73% for RNN between192 and1,152 updates;
every seed increased. None met the predeclared10-point material-gain or95%probe /
90%final-holdout targets. Fresh holdout means were33.92% /32.03%, versus12.5%
uniform/query-key baselines and100% dictionary replay (an exact task reference,
not a matched learned competitor). Earlier peaks were not selected as outcomes.

Initial and loss curves are now retained for this A-only study. Loss fell, but
curves were not monotonic and known-example probe accuracy remained below43%.
Convergence, the cause of limited recall and architectural superiority remain
unresolved. It did not retest STATE-006's B/OOD/overwrite behavior; evaluation
namespaces differ, so cross-study score changes are not same-test improvements.
See [results, the completed recency audit and next control proposal](RESULTS-STATE-007.md),
[status](STATE-007-status.json), [all48 checkpoint records](results/STATE-007/summary.json),
[public pre-run protocol copy](STATE-007.md), [worker](state007.py),
[configuration](STATE-007-config.json) and [nonmodel controls](test_state007.py).

The approved no-training recency audit is now complete. Returning the globally
last SET value scored 135/512 (26.37%) on the same diagnostic set and 161/512
(31.45%) on the final set. All six final diagnostic neural scores beat it; five
final-holdout scores did, with RNN11 below it by nine answers. Final mean margins
were only 2.47 percentage points for GRU and 0.59 for RNN. Dictionary replay was
100%; constant/query-key rules were 12.5%. Both frozen fingerprints and controls
matched, and the owned worker exited. The audit took 12.532 seconds with no model
call or training. Earlier measured learning gains and failed targets are intact.

These are aggregate comparisons, not evidence of what rule a network learned.
Individual predictions/per-delay neural scores were not retained in STATE-007.
Its counterfactual follow-up is now completed in STATE-008 below, with the
original training fixed, weight checks and retained individual predictions.
Candidate for Revia and Integrated remain None.

STATE-008 replayed all six original trajectories and matched all 48 checkpoint
hashes. On 672 new examples per set, final means were 24.26% GRU / 20.63% RNN
against 12.5% constant/query-key controls, 0% latest-write and 100% dictionary
reference. Every seed scored 0/168 fully correct four-case groups and missed
the original screens. Individual diagnostic gains remain measured. No reliable
joint keyed recall, cross-request state or application improvement is established.

All 16,128 predictions are retained locally. Post hoc analysis found the predicted
class unchanged after switching the requested key in 93.45% GRU / 91.17% RNN final
pairs. Rare group-runs reached three of four correct (7/3), preserving partial
sensitivity; this is not a new success gate or proof of training-induced ability.
Argmax stability does not establish hidden-state/logit invariance or lost facts.
Shared groups/seed-runs are dependent, and this challenge differs from training.

The frozen-state reader follow-up is now completed in STATE-009, summarized
above. It compares trained and initial encoder states with trained readers and
a query-blind negative control. These earlier STATE-008 results remain unchanged;
the later readout result does not identify the original failure cause.

See [results and error patterns](RESULTS-STATE-008.md), [status](STATE-008-status.json),
[exact aggregates](results/STATE-008/summary.json), [method](STATE-008.md),
[worker](state008.py), [data/metrics](state008_data.py),
[configuration](STATE-008-config.json), [data controls](test_state008_data.py)
and [worker controls](test_state008.py).
No further training or integration starts automatically.

[READINESS-005](READINESS-005.md) retains its diagnostic repair, socket denial and
permitted-context Python HTTP pass; its0.609s duration is not model latency or
proof of attempt01's historical cause. Positive RNL-003 evidence remains intact.
Candidate for Revia and Integrated remain None. RNL's broader trainable
adaptive/stateful goal remains; these checks qualify one ingredient. No further
run, training or integration starts automatically.

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
