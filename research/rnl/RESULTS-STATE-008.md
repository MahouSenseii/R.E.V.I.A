# STATE-008: partial answers, weak response to the requested key

All six exact training replays completed. Individual final accuracy averaged
24.26% for GRU and 20.63% for RNN, above the 12.5% simple controls. Every trial
scored zero fully correct four-case groups and missed both declared screens.
The result is a useful negative for reliable joint key/value recall on this
new challenge distribution; it does not rule out the broader RNL idea.

All 48 original checkpoint hashes matched. These were the original STATE-007
trained models, not a new architecture or training-data intervention. The
[method](STATE-008.md) and [exact aggregates](results/STATE-008/summary.json)
preserve primary results, controls and the later descriptive error analysis.

## Primary result and reference definitions

Each final set has 672 examples in 168 related groups. Query/value pairs require
two correct answers; a group requires all four. The variants are dependent.

| Model/seed | Correct /672 | Query pairs /336 | Value pairs /336 | Four-case groups /168 |
| --- | ---: | ---: | ---: | ---: |
| GRU 11 |167|1|6|0|
| GRU 23 |163|3|11|0|
| GRU 37 |159|4|16|0|
| RNN 11 |131|1|8|0|
| RNN 23 |139|1|7|0|
| RNN 37 |146|1|10|0|

Constant0/query-key rules score 84/672 (12.5%); global-last-SET scores zero because
the construction makes it wrong. Dictionary replay scores 672/672 and 168/168
groups. It is an exact task reference, not an equally budgeted neural competitor.
12.5% is an individual-answer baseline, not a four-case-group success rate.

Every diagnostic group score was also zero at updates 0, 192 and 1152. Material
signal required a 17-group diagnostic gain, 17 final groups and 168 individual
answers in every seed. Reliability required 605 individual answers and 135 final
groups. Neither screen passed for either model. GRU 11 was one individual answer
below 168, but the zero-group result is not a one-answer threshold issue.

Individual diagnostic gains remain measured. Means at 0/192/1152 were
12.45/17.31/19.54% GRU and 11.36/16.17/18.11% RNN; all six increased. Final-set
performance was measured only at the end, so its improvement was not measured.

## What the individual predictions show

This analysis was performed after the run. It introduces no new success gate.
All 16,128 retained rows were reconciled, without another model call or dataset
generation. The disjoint categories below use the known group targets: requested
answer, the other queried key's answer, the wrong latest value, or another class.
They are post hoc labels, not additional deployable prediction rules.

| Final outputs across three seed-runs | GRU /2,016 | RNN /2,016 |
| --- | ---: | ---: |
| Requested answer |489|416|
| Other queried key's answer |477|415|
| Latest distractor |518|602|
| Other value |532|583|

Switching only the requested key left the predicted class unchanged in
942/1,008 GRU pairs (93.45%) and 919/1,008 RNN pairs (91.17%). When a prediction
matched either of the two candidate answers, the requested one accounted for
489/966 (50.62%) and 416/831 (50.06%). These conditional ratios do not establish
that both facts were internally stored or that the model guessed between them.
Identical predicted classes can conceal changed probabilities/hidden states;
those values were not retained.

On the same diagnostic sets, query-invariant argmax pairs rose from
53.47% to 90.67% to 93.65% for GRU and 65.87% to 85.62% to 92.16% for RNN across
the three checkpoints. Earlier value-swap invariance decreased: GRU
93.06/78.87/75.89%, RNN 75.30/75.10/66.37%. Individual accuracy improved while
class-level response to the requested key remained weak. This is an output
pattern, not proof that training erased key information or identified a cause.

There is useful partial behavior. Seven GRU and three RNN final group-runs reached
three of four correct (out of 504 group-runs/model, sharing 168 groups). A
deterministic query-blind or value-assignment-blind rule cannot exceed two of four,
so these cases show partial sensitivity to both changes. They remain rare, none
reached four, and rare three-of-four cases already occurred at initialization on
the diagnostic set. Initial final-set scores were not measured. Do not call the
final cases a new training-induced capability or relax the original screens.

Per-delay controls matter: delay 7's constant baseline was 16.67%, while pooled
GRU/RNN means were 13.54%/11.11%. Overall improvement above 12.5% does not mean
every delay beats its own baseline. Histories and label mix vary with delay;
these strata do not establish a causal forgetting law.

## Next proposed direction: a frozen-state reader probe

Test whether a small reader can recover the requested fact from the history
state when it receives the query key directly. Freeze the original GRU encoder,
then compare a query-selected reader, a query-blind negative control, and a
query-selected reader over the initial untrained encoder. Use fresh whole-group
probe training/evaluation data, retain predictions and hold readout training
settings fixed. This asks a narrower question about accessible information and
readout than changing the whole training distribution at once.

The supplied query selection would not be learned adaptive routing. Equal
nominal readout parameters would not make the query-blind mean an equally
expressive competitor. A positive result would show decodability under the new
probe supervision, not that the original query transition caused failure. Failure
would not prove information absent: linear readout, budget and distribution
limits remain. Encoder weights were not retained, so exact GRU replays are still
needed; the next bundle should retain safe bounded checkpoints to avoid another
replay cycle. The follow-up now has local source/control preparation; its native path is untested, untrained and not approved for execution.

The current evaluation changes several features of the original training
distribution. Neither these counts nor the proposed probe diagnose optimization,
representation or architecture as the cause. No longer-delay STATE-006 repair,
persistent cross-request state, LLM quality or Revia improvement is demonstrated.
Earlier useful results and failures remain intact. Candidate for Revia and
Integrated remain None.

## Execution and evidence limits

One approved invocation completed six cells / 6,912 updates in 386.645 seconds
including the observer. All 587 pins, 48 weight checks, fixed fingerprints,
limits and cleanup reconciled. All seven owned processes were observed absent.
Peak worker WS/commit was 308,359,168/316,407,808 B; supervisor RAM was separate.
New execution output was 1,149,913 B. Sampled owned/evidence bytes were
708,845,260/5,153,042 before later notes, below 2 GiB/10 MiB. Disk is sampled,
not a quota; no speed or RAM improvement is established. The prior environment
census is pinned, with current path/size checks unable to detect same-size edits.

The summary includes exact public aggregates and source/evidence hashes. All
individual predictions, controller commands, machine paths and operational logs
remain local; no private chat was used. Scientific raw-result SHA256:
`5a3b33e65b3889ea97c87bc2c8a6b98461144e4f1a7284008e543ca8839b21fe`.
Independent result and post hoc arithmetic reviews found no discrepancies.
Public method/code/config/tests preserve reviewed bytes; native execution still
requires an external resource guard. No new experiment or integration starts
automatically from this record.
