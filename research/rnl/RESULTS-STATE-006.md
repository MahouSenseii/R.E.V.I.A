# STATE-006: completed comparison, scientific no-go

The approved RUN06 batch completed in447.6983484seconds: offline setup, actual
isolation/data controls, two no-update probes and all six fixed training cells.
All1,536 updates completed. Independent raw-result review reproduced the
original **no_go** assessment. No result qualifies for Revia integration.

This is a negative result for the specified tiny standard GRU/RNN task, capacity
and learning budget. It neither establishes a novel RNL architecture nor rules
out the broader adaptive/stateful research goal. Earlier RNL-003 synthetic gains
and PREFIX-004's existing-backend reuse remain measured within their own scope.

## What was tested and what chance means

Episodes contain64 events, four keys and eight possible values. A dictionary
oracle defines the latest value for the final queried key. Only SET events carry
value bits; final-query-only supervision supplies one label per episode. Every
512-example evaluation balances all32 key/value combinations:64 examples per
value,16 per key/value. Uniform random guessing has expected12.5% single-query
accuracy; any constant or deterministic query-key-only predictor scores exactly
12.5% on these balanced evaluations. Near this number is descriptive, not a
significance test or proof of zero information.

Each seed/model received3,072 fresh A episodes (lags1-8;192 updates of batch16),
then1,024 fresh B episodes (lags9-16;64 updates). OOD evaluation used lags17-32.
The separate untouched diagnostic has lags60-63. State starts at zero for every
episode: the study tests memory within one sequence, not state carried between
requests, autonomous growth, adaptive expert routing or language-model training.

GRU/RNN capacities were close in parameters (4,968/4,964) and estimated forward
matrix MACs (289,024/279,896), but not exactly equal compute. GRU uses hidden32
and128 state bytes; RNN hidden59 and236 bytes. The3.26% MAC difference excludes
nonlinearities and backpropagation; training FLOPs were not measured. These are
standard neural baselines rather than a custom RNL implementation.

Sources: [data and balanced splits](state006_data.py#L90),
[task lags](state006_data.py#L117), [capacity](state006.py#L23),
[reset/forward behavior](state006.py#L128), [training](state006.py#L186).

## Exact learning and retention observations

Percentages below are rounded for reading; the [summary](results/STATE-006/summary.json)
retains exact fractions, all six scientific records, counts and timing arrays.

| Model/seed | A after192 A updates | A after256 total updates | Final B | Final OOD | A decline, points |
| --- | ---: | ---: | ---: | ---: | ---: |
| GRU11 |30.47%|29.88%|18.55%|12.11%|0.59|
| GRU23 |31.84%|28.13%|18.16%|12.89%|3.71|
| GRU37 |27.54%|25.98%|17.19%|16.21%|1.56|
| RNN11 |23.44%|15.04%|11.72%|10.16%|8.40|
| RNN23 |16.41%|12.50%|12.30%|11.91%|3.91|
| RNN37 |28.91%|19.53%|17.58%|14.26%|9.38|

The GRU OOD advantage was1.953125,0.9765625 and1.953125 percentage points.
The predeclared per-seed gates required95% checkpoint-A/final-A/final-B accuracy,
90% OOD,95% overwrite-pair success and at least10 percentage points OOD advantage.
None of the three seeds met those quality requirements. The two-point maximum
A-decline rule additionally failed for GRU23 (3.7109375 points). It passed for
GRU11/37, but preserving poor accuracy is not sufficient learned memory.

Overwrite is a **pair** metric: both answers must be right after changing the
last relevant SET from v to(v+1)%8. GRU succeeded on0/128,1/128 and3/128 pairs;
RNN on0/128 for all seeds. Independent uniform guesses have expected1/64=1.5625%
pair success; a deterministic predictor giving the same answer on both versions
scores zero because their correct labels differ. Do not compare this metric to
the12.5% single-query baseline or call it ordinary overwrite accuracy.

State-reset accuracy was exactly12.5%, as expected when only final QUERY/key is
visible and labels balance per key. GRU state-exchange accuracy was15.43%,13.09%
and13.28%. The required50-point drop from normal OOD accuracy did not occur; with
normal accuracy near12.5%, that drop cannot establish a useful state effect.
This floor limits interpretation of failed interventions. Clean-reset
consistency was100% for both models: repeatability, not correct recall.
See [assessment](state006.py#L32) and [interventions](state006.py#L257).

## Learning curves: what is and is not known

No loss values, training accuracy curve, initial accuracy or dense held-out
learning curve were retained. Each256-element update_seconds array records
**duration**, not loss. Loss and gradient norms were checked finite and then
discarded. Only two weight checkpoints were evaluated on A: after192 A updates
and after64 additional B updates. B/OOD were evaluated only at final weights.

After192 updates, GRU A accuracy27.54-31.84% was above the12.5% simple
baseline, followed by declining A recall after B exposure. Without initial
accuracy, training-induced improvement is unmeasured. They do not show a rising loss
curve, convergence, plateau, divergence, B learning rate or an adequate training
budget. Mean measured optimization-phase duration was about4.40s for GRU and
2.27s for RNN, including data preparation; those finite timings do not determine
learning adequacy. No deadline truncated optimizer updates.

## Explanations remain hypotheses

| Explanation | Supporting observation | What is missing |
| --- | --- | --- |
| Limited learning exposure | Only4,096 query labels/256 updates; A accuracy was still modest at192 | No retained loss/slope or longer-budget control; fresh data and updates were not independently varied |
| Task/credit-assignment difficulty |64 events, distractor writes, supervision only at final query; longer lag shift from A to B to OOD | No per-lag learning curve, immediate-copy control or matched same-lag continuation |
| Phase interference/retention | A accuracy declined after the64-update B phase | No same-budget A-only continuation or B-only checkpoint curve; observed decline does not identify its mechanism |
| Architecture/optimization | Small standard recurrent models, different state widths, finite gradients but poor recall | No architecture-only or optimizer-only ablation; numerical viability is not successful optimization |

Failure already exists on the original short-lag A task, so OOD difficulty alone
cannot explain all failures. Conversely, this comparison does not establish that
recurrence, gating, persistent-state research or the wider RNL goal is unworkable.
Three paired seeds and these task-specific metrics do not establish a reliable
general architectural superiority, language quality or application benefit.

## Smallest recommended next experiment (proposal only)

**Question:** Can the original short-lag A task become learnable under a larger,
fixed learning-exposure budget? Recommend one seed11 GRU/RNN pair before any new
architecture or long-delay study. This keeps the measured task intact; a lag1
immediate-copy control is smaller, but changes task difficulty and would answer
a narrower question when short-lag accuracy already exceeds the simple baseline.

Keep model sizes, encoding, optimizer, learning rate, batch16,64-event inputs,
CPU/runtime and initial seeds unchanged. Reuse the exact original3,072 train_A_11
episodes for the first192 updates; each model's checkpoint-A weight hash must
match its original record. Do not enlarge that split in place: its balanced
shuffle depends on count. Then continue with13,312 disjoint A episodes (lags1-8)
under a separately frozen namespace, reaching1,024 total updates. No B phase.
Both models receive identical batches and update counts; retain the original
capacity/compute caveats.

Record online batch loss/accuracy and fixed-set diagnostic measurements at
0/32/64/128/192/256/512/1024 updates. Define a fixed128-example balanced probe
from the original A-training pool (first four episodes for each key/value in
original order); it is not a held-out set and may precede exposure at early
checkpoints. Use a separate fixed512-example diagnostic-validation split and a
fresh sealed512-example final A holdout, evaluated once at update1024. Evaluate
without gradients or optimizer/RNG changes, restore training mode, and retain
all curves. No early stopping, checkpoint choice, tuning or extension from them.

Report each model separately. Proposed basic-learnability screening requires
at least95% on the training-pool probe and90% on the fresh final A holdout; keep
uniform/key-only12.5% baselines and the exact dictionary oracle as a non-neural
positive control. These proposed thresholds do not replace STATE-006's original
gates or show superiority, long-delay memory, overwriting, forgetting or
integration readiness. One seed is a screen, not a statistical comparison.

Fresh examples and optimizer updates increase together. This does **not** isolate
insufficient data from insufficient steps, nor diagnose the original B/OOD cause.
If basic A learning succeeds, separately qualify delay/phase changes; if it stays
poor, review representation/optimization before larger training. Both outcomes
are useful; do not automatically train further.

Reuse the installed isolated CPU environment and current harness patterns;
no new framework, dependency, GPU or private data. Proposed bounds are one CPU,
2GiB per worker,2GiB combined retained/new disk,10MiB combined execution evidence and one10-minute
overall window including15s cleanup. Existing runtime observations suggest this
is small, but exact feasibility/commands, pinning and cleanup require review
before a separate execution approval. Stop on a failed control, nonfinite metric
or resource/cleanup failure; no retry or automatic repair. No such experiment
was implemented or executed during this analysis.

## Method amendment, resources and cleanup

RUN06 used unchanged scientific/data bytes and separately approved new execution
orchestration: one2,658-second window including15s cleanup, carrying42s for known
prior invocations. It prospectively replaced overlapping phase/active timers,
full-time admission and inter-phase review receipts. Earlier failures remain
failures. The historical [STATE-006 protocol](STATE-006.md) retains its original
timers; this paragraph describes the effective RUN06 policy. Coordinator/guard
source and exact request are retained privately by hash in the summary and are
not part of this documentation-only payload.

Actual isolation, eight original data/science controls, official cached-wheel
size/SHA/archive verification, offline dependency closure and both no-update
probes passed. The probes recorded finite gradients, zero optimizer steps and
identical before/after weight hashes. All13 owned job records show exit0,
nontruncated output, one-CPU affinity, verified native limits and cleanup.
The previously passed eight disk-sampler fixtures were not repeated.

Installed files total538,492,227B across15,567 files, with final path/size checks
matching the full post-install census. Peak worker commit/working set was
316,751,872/307,367,936B. Terminal owned bytes were702,601,536B and execution
evidence1,667,041B, before small post-run records. RAM caps exclude coordinator;
disk accounting samples logical bytes and is not an OS quota or an exact
instantaneous peak. Training FLOPs, deployment RAM savings and speedup are unknown.

All20 critical pins,45 preserved files and four original environment pins matched.
All13 children and the coordinator exited. An initial exact14-PID lookup found
none present; later reuse of the former coordinator PID by an unrelated Node
process was identified and left untouched. Two initial reporting-only scratch
folder warnings were preserved and corrected without repeating the study.
No network download, global-Python change, GPU, private conversation, security
setting, application integration or publication was part of RUN06/this analysis.

## Historical failures stay visible

The first setup attempt took5.5109772s and stopped at pip bootstrap exit101 before
framework installation or neural work. The launcher/process-limit explanation
remains a strongly supported inference; the exact original Windows error was not
retained. Its original measurements, null learning fields and raw-file hashes
are preserved under historical_attempt_01 in the summary and in the
[unchanged published report revision](https://github.com/MahouSenseii/R.E.V.I.A/blob/1c3ff65ea0b79f9594b1735d20e619624e96c3ed/research/rnl/RESULTS-STATE-006.md).

Later local continuation attempts exposed short self-imposed materialization and
prelaunch-admission failures; no training metric was produced by them. Known
30.0814912s,1.6534261s and9.6030097s invocations were conservatively charged42s
before RUN06. One earlier fixture required a full10s worker plus3s reserve inside
15s, leaving only2s for preflight; it failed before launching that worker.
Original files/receipts remain private and unchanged. The new successful run
was separately authorized; it does not turn earlier failed gates into passes.

## Evidence and publication boundary

The sanitized summary preserves all six raw scientific records and both probes,
original assessment, prior setup evidence, chance derivations, policy identities
and limits. Local source/raw logs retain private paths and process details; they
are not copied into public content. Key private evidence hashes:

- Raw RUN06 result:4d10f420ded93e3f019a4bda8065644d3ce8050a243646c2552220de6c1327a1.
- Independent result review:6998ecda892e23188c30c7e083ae8bad4df2762f1590a49f277c3813ce73b262.
-1129-byte controller capture:bb6d58fa6db710462a8fdaccc0d2093ce46fcfdd7c0b8f7dd80d7571d987f55d.
- Installed census:bab00163b9c3b963c4e865344ded3b4b464dfa0b76696b0f785f71e6eac4e853.

Candidate for Revia and Integrated remain None. Research content belongs on
research/rnl-sandbox; the sole Atlas target is docs/atlas/research.html. No other
Atlas page, metric, shared asset, workflow or application file is included.
No future experiment or application integration is authorized by this report.
