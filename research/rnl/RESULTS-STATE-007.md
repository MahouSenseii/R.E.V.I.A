# STATE-007: modest learning progress, stronger targets missed

More updates on the same short-delay examples helped both standard recurrent
models in every paired seed. The improvement was modest: none met the declared
10-point material-gain or basic-learnability screens. This is a useful measured
positive direction with weak recall, not a validated RNL architecture or a Revia
integration candidate. The original STATE-006 no-go remains unchanged.

One approved CPU batch completed six cells, 6,912 optimizer updates and 48 fixed-set
checkpoints in 360.417 seconds. There was no model retry, tuning or further run.

## Results and comparisons

Each cell repeated its original 3,072 A examples in the same order for six passes:
1,152 updates, batch 16, 64-event episodes and SET-to-QUERY delays 1-8. Models,
optimizer and encoding stayed fixed. Both models shared the pool within each
paired seed, and all six original update-192 weight hashes matched. The three
paired seeds vary initialization and original training pool; evaluation sets are
shared. Nothing was selected from the curves.

The diagnostic comparison below uses the same 512 balanced examples at both
checkpoints. Fresh final holdout is a separate set, evaluated once per cell at
1,152. Its improvement over 192 was not measured.

| Model/seed | Diagnostic at 192 | Diagnostic at 1,152 | Gain, points | Fresh final holdout |
| --- | ---: | ---: | ---: | ---: |
| GRU 11 |30.47%|31.64%|1.17|33.79%|
| GRU 23 |30.27%|34.38%|4.10|34.77%|
| GRU 37 |31.05%|32.81%|1.76|33.20%|
| RNN 11 |23.05%|31.25%|8.20|29.69%|
| RNN 23 |18.95%|28.13%|9.18|31.84%|
| RNN 37 |25.98%|32.81%|6.84|34.57%|

Mean gains were 2.34 points for GRU and 8.07 for RNN. Mean fresh final accuracy
was 33.92% / 32.03%. Uniform guessing has expected 12.5% accuracy, the deterministic
query-key rule scored exactly 12.5%, and dictionary event replay scored 100% on
every recorded control set. Replay is an exact task reference, not a compute-matched
learned competitor.

Observed all-seed increase passed for both models. Material gain required at
least 52 additional correct diagnostic answers out of 512: GRU gained 6/21/9 and
RNN 42/47/35, so none passed. Basic learnability required at least 122/128 correct
on the fixed training-pool probe and 461/512 on the final holdout, for every seed.
Probe counts were GRU 54/47/49 and RNN 48/52/49; final counts were GRU 173/178/170
and RNN 152/163/177. All missed those thresholds. A missed screen does not prove
no progress, and no threshold was relaxed after the result.

## Learning curves

These are means across three paired seeds. Loss is fixed-set cross-entropy;
lower values indicate better probability fit. The [evidence summary](results/STATE-007/summary.json)
retains all 48 exact per-seed checkpoints, including training-probe accuracy/loss,
weight hashes, final results and every screening decision. Online batch arrays
remain private with their raw stream hashes; they are not substituted for fixed-set curves.

| Updates | GRU diagnostic accuracy | GRU loss | RNN diagnostic accuracy | RNN loss |
| ---: | ---: | ---: | ---: | ---: |
|0|13.15%|2.0886|12.11%|2.2150|
|32|17.45%|2.0557|15.95%|2.0947|
|64|21.74%|2.0103|19.40%|2.0428|
|128|26.63%|1.9154|22.27%|2.0188|
|192|30.60%|1.8416|22.66%|2.0077|
|384|33.53%|1.7658|26.95%|1.9209|
|768|32.75%|1.7322|28.84%|1.8733|
|1152|32.94%|1.7378|30.73%|1.8647|

Fixed-probe and diagnostic loss ended below their 192-update values in every cell.
Final mean training-probe accuracy was only 39.06% / 38.80%, versus diagnostic
32.94% / 30.73%. Thus poor final performance is not established as overfitting
alone; even the sampled known examples were not reliably recalled.

Progress was not monotonic. GRU 11 reached 173/512 diagnostic correct at 384 but
ended 162; GRU 37 reached 172/512 at 768 but ended 168. RNN 23 also dipped late.
Earlier peaks are not selected outcomes. Eight sparse checkpoints do not establish
convergence, a plateau, an adequate budget or the cause of limited recall.

## Difference from STATE-006

STATE-006 trained 192 short-delay A updates, then 64 longer-delay B updates
(delays 9-16), and evaluated OOD delays 17-32 plus overwrite/state controls.
STATE-007 repeated only A delays 1-8. It did not retest B/OOD retention or overwrite.
The initial A weight checkpoints match, but evaluation namespaces/seeds differ
(6006 in STATE-006 versus 7007 here). Higher final STATE-007 scores are not proof
that the original longer-delay failures were repaired.

This new study resolves the missing initial/loss-curve evidence for its own task.
It supports modest benefit from more repeated updates at fixed unique-data size.
Repeated exposure and compute still increase together. It does not establish
that insufficient updates alone caused the earlier failure. Standard GRU/RNN
state resets each episode; persistent memory across requests, adaptive routing,
language quality, a new RNL architecture and application savings remain untested.

RNN gained more from 192, while GRU final holdout wins two seeds and loses one.
Three paired seeds/shared evaluations and approximate capacity matching do not
establish statistical or general architectural superiority. Parameters were
4,968/4,964, with about 3.26% different estimated forward matrix work; training
FLOPs were not measured. Candidate for Revia and Integrated remain None.

## Smallest recommended next step: audit the recency baseline

Before another neural trial, reconstruct only the existing frozen STATE-007
diagnostic/final sets and compare a stronger simple rule: return the value of the
globally last SET, ignoring the query key. Require both recorded dataset
fingerprints to match; use no new dataset, Torch, model call or training.

This comparison is currently missing. After the target SET, the generator adds
other-key writes or NOOPs. If every intervening event is a NOOP, the global last
SET is still the answer. Otherwise a random distractor value can match by chance.
For the intended generator, the analytic expectation at delay L is
1/8 + (7/8)*(0.3)^(L-1), about 28.124% when delays1-8 are uniform. This is NOT a
measured finite-set score and must not be used to rank the recorded networks.
The relevant source is [event generation](state006_data.py#L58).

The proposed audit compares constant, query-key-only, global-last-SET and
independent dictionary-replay rules using observable events. Report exact overall
and per-delay counts/accuracy. Constant/key-only scores are 12.5% overall on these
balanced sets; individual delay strata need not preserve exact label balance.
Compare the measured overall rule scores with the existing neural aggregates.

This tests whether the networks exceeded a stronger key-blind benchmark. It
cannot show that a network actually uses recency: neural predictions and per-delay
neural scores were not retained, so error overlap/paired-correctness comparisons
are unavailable. Training gains remain measured regardless of that audit.

Only a small standard-library script would be needed. Proposed bounds are one
CPU,256MiB worker RAM, a single60-second overall window including cleanup, and
the existing2GiB combined disk/10MiB combined evidence ceilings, with new audit
output at most256KiB. Exact code, input pins, resource feasibility and command
still need review and applicable execution approval. The audit is unimplemented
and unrun; do not regenerate episodes or evaluate rules as part of this update.

A two-cell lag1 GRU/RNN copying control can remain a later option, not an automatic
next run. At lag1, the key-blind last-SET rule is100%, so passing would show only
immediate copying. Changing lag also changes generator random draws/history,
so it would not be a pure causal delay ablation. Neither proposal diagnoses the
cause of limited recall or qualifies longer/keyed memory.

## Controls, resources, history and reproducibility boundary

Independent review reconciled all six raw cells, all prefix hashes and finite
online observations, the exact eight checkpoint order, noninvasive evaluations,
paired identities, baselines, final-once gates and screening counts. All six
worker guards exited 0, verified one-CPU and 2 GiB limits, bounded output and
confirmed owned exit. The coordinator also exited; all seven recorded owned
PIDs were observed absent. All 22 critical and 442 prior-preservation pins matched.

The 1,200-second limit included 15 seconds cleanup. Observed wall was 360.417 s.
Peak worker commit/working set was 317,587,456 / 309,637,120 bytes. Terminal owned
accounting before final/post-run records was 706,270,823 bytes and execution
evidence 2,578,605 bytes, below 2 GiB / 10 MiB. RAM excludes coordinator; disk is
sampled logical accounting, not a filesystem quota or instantaneous peak.
The prior environment census is pinned; current path/size checks do not detect
same-size package edits. No installation, download, GPU, private conversation,
live model service, security-setting or application change occurred.

The [public protocol copy](STATE-007.md) labels its historical preparation status
and omits machine-specific paths. [Worker](state007.py), [configuration](STATE-007-config.json)
and [tests](test_state007.py) preserve their reviewed bytes. Eight nonmodel controls
passed without importing PyTorch before the run. The machine-specific controller,
resource wrapper, raw operational logs and complete online arrays remain local
by hash. This is an evidence/method record, not a self-contained deployment package.

The original STATE-006 no-go, setup/admission failures, RNL-003's positive toy
result and PREFIX-004's real-model reuse are retained unchanged. No publication
or subsequent experiment is authorized by this report. Any public update is
limited to the separate research branch and the dedicated Atlas research page.
