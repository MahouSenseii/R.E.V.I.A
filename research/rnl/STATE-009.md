# STATE-009 method: frozen-state readout qualification

Post-run method record. The scientific configuration, data identities, original
weight references, resource controls and decision screens were frozen locally
before the single approved invocation. This explanatory file and the later error
analysis are prepared after measurement; the error analysis is not preregistered.

Question: can a small supplied-query reader jointly recover stored facts from a
fixed recurrent state, and how does that readout compare over trained versus
initial encoder features? This is a synthetic memory probe, not language-model
training, a replacement backend or an explanation of the original failure cause.

## Fixed training and feature boundary

- Original GRU seeds 11/23/37: 3,072 original examples and 1,152 updates per seed,
  unchanged ordering/Adam/loss. All 8 original checkpoint hashes per seed had to
  match: 24 total. Initial and final full-model snapshots were retained.
- Freeze and clear encoder gradients; use raw 32-dimensional CPU FP32 states after exactly
  63 history events, before QUERY. Reset state per history; deduplicate/copy the
  same history feature for both query variants. No target or final query enters
  the encoder cache. Preserve encoder weights/mode/gradients and CPU RNG.
- New reader training: 4 fresh 168-group blocks, 2,688 episodes, 6 passes, batch 16,
  1,008 updates per reader. Same examples/order, fixed Adam learning rate 0.003 and zero weight decay,
  clip 1. Reader initial tensors match within each paired seed; optimizers are
  independent. No best-checkpoint, hyperparameter, seed or final-score selection.

## Three arms, nine readout fits

1. Trained encoder + supplied-key selected reader.
2. Trained encoder + query-blind mean reader.
3. Initial encoder + supplied-key selected reader.

Each reader computes four 32-to-8 linear heads: 1,056 nominal parameters and 1,024 main
forward matrix MACs. The observable query selects one head, or the blind arm
averages all four. Selection is supplied, not learned routing. The blind mean
collapses to one affine map; nominal parameter equality does not make it equally
expressive or isolate the value of query information. Gather/mean, gradient and
optimizer work differ; no equal-total-compute claim is made. Initial-encoder
readers are trained readers, not wholly untrained models.

## Data, evaluation and unchanged screens

Version `STATE-009-quartet-v1`, seed 9009, six namespaces in
[the executed configuration](STATE-009-config.json). Every block/set contains
168 related four-case groups/672 episodes: two queries with distinct answers,
then the earlier values exchanged, while the final distractor write stays fixed.
The last-write value differs from both requested values. Keep whole groups and
observable histories separate among splits and from 11,584 prior reference episodes.
Preparation verified 4,032 new episodes/1,008 groups/2,016 unique histories.

Training/diagnostic curves at 0/168/1008 updates; training probe is the first
672-example training block and online summaries cover all updates. Retain
individual rows for the final diagnostic and once-only final evaluation at 1008:
12,096 rows across 9 readers. Final features are extracted after each relevant fit;
scores do not change training. Rows identify group, assignment, query, key, lag,
target, last-write value and prediction. Logits are not retained.

The query-blind control must have identical logits/classes for identical history
features and zero fully correct query-pairs/quartets. A violation invalidates the
run; zero group success is expected for this control, not evidence against all
learned memory.

Unchanged pragmatic screens, required in every trained-selected seed:

- Exploratory signal: 17/168 final quartets and 168/672 individual answers.
- Trained-feature advantage: 17 additional final quartets over initial features.
- Stronger reliability: 135/168 quartets and 605/672 individual answers.

Report all negative/mixed/incomplete results. These are not significance or
integration gates. Uniform individual expectation and overall constant/query-key
controls are 12.5%; last-SET 0%, exact dictionary 100%. Per-delay finite proportions
differ. Dictionary is a task reference, not a compute-matched learned competitor.
Independent-uniform quartet expectation 1/4096 is not a calibrated neural null.
Variants and repeated seed-runs are dependent.

## Execution and public evidence boundary

The approved invocation completed in 305.576 outer seconds, no retries. Three
sequential workers had one logical CPU/2 GiB commit and hard-working-set limits,
with 1,200 seconds overall including 15 seconds cleanup. Retained/new data counted
against 2 GiB owned disk/10 MiB evidence and 2 MiB new execution output. Supervisor
memory was separate; disk was sampled accounting. Stop on identity, data,
numerical, control, resource, artifact or cleanup failure. No failed-cell relaunch,
cap extension, installation, GPU, private data or application changes.

Fifteen checked finite little-endianFP32 files retain six full GRUs and nine
readers: 157,248 raw parameter bytes plus bounded JSON inventories. These and the
full prediction rows stay local. Public aggregates include their hashes and
inventories, not weight bytes. The raw result hash is
`6aab4c7e3f9b0109f07c46a5b0c64c65f9579875a70e1a4a832b8c38b61cde57`.

The exact executed [worker](state009.py), [data snapshot](state009_data.py),
[worker controls](test_state009.py), [data controls](test_state009_data.py) and
configuration are byte-preserved. Historical prepared/future wording in source
describes its inert default CLI. Native execution still needs an external reviewed
resource guard; the private Windows controller/commands are not published. This
is not a self-contained native-run package. See [results](RESULTS-STATE-009.md),
[status](STATE-009-status.json) and [exact public aggregates](results/STATE-009/summary.json).
