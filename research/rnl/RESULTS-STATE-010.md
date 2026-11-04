# STATE-010 results: no nonlinear-reader advantage demonstrated

Nine reader fits completed successfully in one 216.566-second CPU invocation.
All source encoders stayed frozen. None of the nine fits recovered a complete
held-out four-case group; the nonlinear reader missed every fixed screen.

| Correct answers /672 | Seed 11 | Seed 23 | Seed 37 | Descriptive pooled /2016 |
|---|---:|---:|---:|---:|
| Direct linear |177|191|194|562 (27.88%)|
| Factorized identity |193|203|200|596 (29.56%)|
| Factorized ReLU |186|187|190|563 (27.93%)|

Each cell has 0/168 complete quartets, or 0/504 per reader pooled over seeds.
Query-pair successes are 32/35/28 and value-pair successes 27/29/28, each/1008
for direct/identity/ReLU. ReLU-minus-direct answer counts are +9,-4,-4; versus
identity,-7,-16,-10. These dependent shared-group counts are descriptive.

The exact constant/query-key references score 12.5%, last-write zero by design,
and dictionary replay 100%. Dictionary uses supplied symbolic keys/operations
and is an exact task reference, not an equally budgeted learned competitor.
Finite per-delay baselines differ; no calibrated neural-null claim follows.

## Learning and saved errors

Diagnostic answers increased from 66–94 initially to 174–189/672 at 1008 updates.
Training-probe answers increased from 70–101 to 195–229/672. Losses improved, but
this does not demonstrate convergence. Individual performance above chance
does not establish reliable binding or correct overwriting.

Post hoc saved-row audit: query changes leave the class unchanged in 696/1008
direct pairs, 758 identity, 706 ReLU (69–75%); value swaps in 777/796/774 (77–79%).
Correct query/value pairs remain rare. Counts of 0/1/2/3/4 correct answers per
quartet are 168/125/196/15/0 direct, 167/97/221/19/0 identity, 175/106/212/11/0 ReLU.
Rare partial successes remain useful, not a replacement success screen.

Equal classes can conceal changed logits, and output invariance does not
establish equality or absent information in hidden states. Value swaps may
change history states. These findings do not identify a hidden cause. No
population interval is claimed from structured variants and just three seeds.

## What the comparison establishes

Under this recipe/budget, ReLU has no consistent advantage over direct linear
and scores fewer individual answers than identity in all seeds. Every signal,
advantage and reliability screen is missed. This rules out neither nonlinear
readers generally nor a workable RNL architecture. Encoder/training,
optimization and other decoder limits remain unresolved.

The direct reference guards against mistaking recovery of factorization loss
for an improvement. Its parameterization/initialization differ from the paired
factorized readers; Adam settings match. Nearly matched parameter and main-MAC
counts do not match full compute. Supplied query selection is not learned routing.

All 814 frozen identities, 63 evaluations, 12096 retained predictions, 15 finite
FP32 snapshots and owned cleanup reconciled in independent review. Peak worker
working set/commit were 307138560/313090048 B under 2 GiB; one-CPU caps and the
1200-second total bound held. This is operational accounting, not a RAM/speed gain.
No encoder training, dependency install, GPU run or Revia integration occurred.

## Next decision, design only

The bounded decoder follow-up is finished. A materially different lead is a
small learned keyed write/read state, compared with a fresh end-to-end GRU on
the same counterfactual/overwrite recipe and a simple external exact dictionary.
The dictionary already solves this explicit symbolic task without training;
learning adds value only if its tested capability justifies the extra cost.
State/parameter/compute costs and supplied symbolic structure must be explicit.
The evidence does not select an architecture or prove that this alternative works.

No next model, training, publication or integration starts automatically.
See [method](STATE-010.md), [status](STATE-010-status.json),
[exact aggregates](results/STATE-010/summary.json), [configuration](STATE-010-config.json),
[worker](state010.py), [data](state010_data.py), [worker controls](test_state010.py)
and [data controls](test_state010_data.py). Full rows/weight bytes stay local.
