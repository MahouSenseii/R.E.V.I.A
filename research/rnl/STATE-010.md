# STATE-010: linear versus nonlinear readers over saved states

Executed once; all fixed quality screens missed. This public method records the
completed study. It is not an execution package or authorization to run one.

## Fixed comparison

Load the three trained GRU snapshots retained by STATE-009, seeds 11/23/37.
Strict finite little-endian FP32 inventories, file/parameter hashes and CPU
loads were checked. Freeze all encoder weights and gradients. No encoder
training or replay occurs. Cache raw 32D states after 63 history events, before
QUERY; reset per history and share vectors for query-only variants.

| Reader | Parameters | Main matrix MACs/example |
|---|---:|---:|
| Four direct 32-to-8 affine heads | 1056 | 1024 |
| Shared 32-to-16 affine map, identity, four 16-to-8 heads | 1072 | 1024 |
| Same factorized map with ReLU | 1072 | 1024 |

Compute every head and select using the observable query key. This is supplied
selection, not learned routing. Identity/ReLU share exact initial tensors and
data/order/optimizer/update budget; activation is their only treatment change.
Direct has a different parameterization and separately fixed initialization.
Factorization changes rank and optimization geometry. ReLU adds 16 elementwise
operations; bias, selection, backward, optimizer and feature extraction differ.
Do not infer equal total compute or a speed comparison from main matrix MACs.

Direct initialization seed is 1001000 plus encoder seed; factorized seed is
1001100 plus encoder seed. Preserve CPU RNG, encoder weights, features and
evaluations. Retain direct/factorized initial and all final reader identities.

## Data and fitting

Version STATE-010-quartet-v1, seed 10010. Four training blocks and separate
diagnostic/final sets each have 168 related groups/672 episodes. Whole groups
and observable histories are separate among all six sets and 15,616 earlier
reference episodes. Held-out means fresh histories in the same four-key,
eight-value task; it does not mean new vocabulary or general language ability.

Each quartet queries two keys in two versions of a history with their values
swapped. Features stop before QUERY and receive no target/candidate metadata.
Train every reader on 2688 episodes for six full passes, batch 16, 1008 updates,
Adam learning rate 0.003, no weight decay, gradient clipping 1. No tuning,
best-checkpoint selection, seed selection or early quality stopping.

Record training-probe/diagnostic curves at 0/168/1008 updates. The training probe
is the first 672-example training block. Only after all three fits per seed,
extract final features and evaluate each final reader once. Retain diagnostic
and final individual predictions (12096 rows), complete pair/quartet/delay
counts, losses, failures/nulls and timing. Negative results remain evidence.

Scalar Python references check native direct, identity and ReLU math. An
evaluation-only blind mean on one complete diagnostic group checks identical
query-only logits and zero full query pairs/quartets before/after fitting.
This is a pipeline invariant, not a trained blind performance comparator.

## Predeclared screens and baselines

ReLU signal requires at least 17/168 final complete quartets and 168/672 answers
in every seed. Advantage additionally requires at least 17 more quartets than
each paired identity and direct reference in every seed. Strong reliability
requires 135/168 quartets and 605/672 answers. These are pragmatic descriptive
screens, not significance tests or integration gates. All were missed.

Overall uniform expectation and finite constant/query-key controls are 12.5%.
Last-write scores zero by construction; dictionary replay 100%. Dictionary is
the exact task reference, not a compute-matched learned model. Per-delay finite
baselines differ. Shared groups, variants and seeds are dependent; independent
uniform quartet expectation is not a calibrated neural null.

## Resource and evidence boundary

Existing Python 3.12.10/PyTorch 2.15.0+cpu environment. Three sequential workers,
one logical CPU affinity and 2 GiB worker commit/hard-working-set caps. One 1200-second
deadline includes 15 seconds cleanup; stop on identity/data/finite/control/resource/
artifact/cleanup failure, no retry or cap increase. Owned disk 2 GiB and execution
evidence 10 MiB include previous retained work. Disk is sampled, not a quota;
supervisor memory is separate. New output cap 1 MiB, per-worker stream 212735 B.

The single run took 216.5655629 seconds. All controls, source identities, finite weights,
output caps and owned cleanup passed. Fifteen fixed-format non-pickle FP32
reader snapshots total 63936 B. Full predictions, weights and operational
controller remain local. Published source/config/tests are the exact executed
versions and reuse the existing research helpers; safe source weights and
external resource supervision are also required for native execution.

The retained nonfatal missing-NumPy warning caused no installation or retry.
No model server, GPU, private conversations, installed Revia model or product
integration was involved. Source identities and exact aggregates are in
[summary.json](results/STATE-010/summary.json); [results](RESULTS-STATE-010.md)
record the limits of interpretation.
