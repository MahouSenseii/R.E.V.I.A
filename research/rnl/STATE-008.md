# STATE-008 method record

This post-run summary describes the frozen pre-execution design, configuration
and executed source. It does not retroactively register the later error analysis.
The completed result is in [RESULTS-STATE-008.md](RESULTS-STATE-008.md).

Question: do the fixed STATE-007 models answer according to both the requested
key and earlier stored values when the global latest write cannot answer?

## Fixed training replay

Final weights were not saved in STATE-007, so the study replayed its six original
CPU GRU/RNN trajectories: paired seeds 11/23/37, the same original 3,072 A episodes
per seed and order, batch 16, six passes and 1,152 updates per cell. Architecture,
initialization, CPU FP32, Adam learning rate .003, weight decay 0 and gradient
clip 1 remained fixed. State resets for every 64-event episode. Original weight
hashes at 0/32/64/128/192/384/768/1152 must all match; mismatch invalidates the
comparison and stops remaining cells. There is no architecture or training-data
intervention. Parameters are 4,968/4,964, with about 3.26% different estimated
forward matrix work; training FLOPs are not identical or measured.

## Counterfactual evaluation

Each new split has 168 groups, one per unordered key pair (six possibilities)
and unordered answer-value pair (28). Each group contains four variants:

| Earlier assignments | QUERY A | QUERY B |
| --- | --- | --- |
| A=a, B=b |a|b|
| A=b, B=a |b|a|

Only the two designated SET value fields change between assignments; only the
last QUERY key changes between query variants. All other events are identical.
A final other-key SET has a third value different from both answers. Designated
key delays are distinct values from 2–8. Every ordered delay pair appears four
times per split, giving 96 examples per delay and 21 per (query key, answer value).
Per-delay labels need not be balanced.

The standard-library generator initializes four keys, uses independent seeded
background writes/NOOPs, forbids later writes to a key after its designated SET,
clones the base history and makes only the intended changes. It does not call
the old target-dependent predecessor-rewrite generator for each variant.
Metadata and targets never enter the neural encoder; only observable events do.
Exact ordering/seed derivation is in [state008_data.py](state008_data.py).

New diagnostic/final namespaces use root seed 8008. Whole related groups stay in
one split. Their 672 distinct observable histories across both splits do not
overlap each other or any of the 9,216 original training episodes. Canonical
fingerprints are SHA256 over each sorted compact episode JSON plus newline:

- Diagnostic: `41b5a5e5a6491d9c3b8d9f70ddc72d27145bcece48735b4b826c8d79e66f5eec`.
- Final: `c029642824a83dfb6ee9325c6fc1e78816374dbc7b03cb6b60a5b9afcd70b36d`.

Diagnostic predictions are retained at updates 0/192/1152. Final neural evaluation
occurs once per cell at 1152. Evaluation uses no gradients/RNG operations and
verifies weights, mode and RNG preservation. Compact rows identify group index,
assignment/query variants, key, delay, target, last SET value and predicted class.
The six cells retain 16,128 rows. Exact public aggregates are preserved; individual
rows and operational logs remain local by hash.

## Controls, chance and predeclared screens

Constant0 and query-key-as-value controls score 84/672 (12.5%); global-last-SET
scores 0/672; independent dictionary replay scores 672/672 and 168/168 quartets.
Dictionary replay is an exact task reference, not a compute-matched learned
competitor. It is based on observable events, unlike post hoc answer-category
labels used only for error analysis.

Uniform eight-value guessing has expected individual accuracy 12.5%. Four
independent uniform guesses would all succeed with probability 1/4096; that is
not a calibrated null for dependent deterministic model variants. Query/value
pairs require both correct answers; a quartet requires all four. Related examples
and seed-runs share groups and must not be counted as independent observations.

Separately for each model, the exploratory material-signal screen requires every
seed to gain at least 17 diagnostic quartets over initialization, achieve at
least 17 final quartets and 168/672 final answers. The stronger reliability screen
requires 605/672 individual answers and 135/168 quartets per seed. These pragmatic
research screens are not significance tests or Revia integration gates. Preserve
all smaller/mixed gains and misses. Do not select a best seed/checkpoint or adjust
thresholds after results. Quality misses alone do not skip remaining fixed cells.

Stop on source/data/weight mismatch, malformed/nonfinite output, resource failure
or uncertain owned cleanup. One invocation, no retry, repair or cap increase.

## Scope and reproducibility boundary

The CPU-only batch has a 1,200-second total window including 15 seconds cleanup,
one active worker/one logical CPU and 2 GiB private commit/hard working set per
worker. Supervisor RAM is separate. Retained/new owned disk and evidence remain
under 2 GiB/10 MiB, with 2 MiB new execution output and 256 KiB per-worker capture.
Disk is sampled accounting, not a filesystem quota or instantaneous peak.

The [worker](state008.py), [data/metrics](state008_data.py), [configuration](STATE-008-config.json)
and two stdlib test files retain their reviewed/executed preparation bytes. The
configuration's preparation flag is historical; current status is in the result.
Twenty nonmodel tests and integration checks preceded the approved execution.
The Windows controller/environment, full prediction rows and operational logs
remain private by recorded hashes. This record is not a self-contained installed
execution package. No installation, GPU, private chat, application change or
automatic model update is part of it.

The later error-bin/invariance/3-of-4 analysis is explicitly post hoc. It does not
replace the original four-of-four screens or establish an internal mechanism.
