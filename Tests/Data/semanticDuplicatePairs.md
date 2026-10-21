# Semantic duplicate advisory corpus

`semanticDuplicatePairs.json` contains 72 manually labeled pairs: 40 development and 32 held-out. The 24 pairs from `Tests/aiPipelineLive.cpp::RunReconciliationLive` retain exact text, original pair IDs, source and original labels. That existing live similarity assay is unchanged.

Four legacy positive assumptions are explicitly labeled uncertain for equivalence: frequency versus preference, night-owl habits versus night work, ongoing versus long-term duration, and reading science fiction versus enjoying novels. Their original labels remain in `legacy.originalLabel`; each reason is recorded in `note`.

Only `equivalent` is a potential advisory mark. `distinct` and `uncertain` must retain separate facts without a mark. Even equivalent examples may be declined by deterministic prechecks, candidate limits, scope, revision history, invalid/truncated model results or the fixed 0.98 entailment / 0.01 contradiction thresholds. False negatives are preferable to marking materially different claims.

The development cases cover opposing preferences, negation, subjects, time, quantities, units, scope, attribution, uncertainty and explicit corrections. Held-out examples use separate claims in the same categories plus positive paraphrases and ambiguous inferences. Preserve this partition; do not choose thresholds on the held-out results and then report those results as independent validation.

`RunSemanticDuplicateTests` consumes this corpus to verify its structure, all 24 legacy references, the frozen split and explicitly required precheck rejections. That test uses no model and does not measure NLI accuracy. Before enabling the default-off feature, independently score and review the corpus with the pinned local model, report development and held-out false marks separately, examine every distinct/uncertain mark, and measure actual runtime/resource cost. The corpus alone is not an activation gate or a claim that broader conversational equivalence has been solved.
