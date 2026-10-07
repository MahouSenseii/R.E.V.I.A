# Foundation and cognition delivery: 2026-10-07

Approved scope is FND-01, FND-02, FND-03 and COG-01 from the owner's complete
upgrade plan. This delivery implements those four items; the remaining roadmap
items and a broad intelligence qualification are outside this acceptance.

## Delivered behavior

| Item | Connected implementation |
|---|---|
| FND-01 | Versioned task contracts and immutable evidence bundles compose existing runtime identity, memory scope, budgets and deliverables. Session/action adapters validate before publication and effects. Contracts grant no authority. |
| FND-02 | Campaign manifests pin source, build, provider, settings, corpus, oracle, hardware and seeds. The registry runner discovers actual CTest selections, builds the selected targets and retains immutable receipts. Empty or missing selections fail. |
| FND-03 | EvidenceJournal owns durable intent/result records and scoped projections. ActionAuditLogger uses that owner; runtime lifecycle metadata uses bounded telemetry. Recovery never repeats an executor, and unresolved effects block dependent effects in the same task/scope. |
| COG-01 | Independent expected answers, strict structured-answer oracles, bound per-sample evidence and separate mechanical/semantic/personality accounting establish a reproducible baseline. Missing, unavailable and unjudged samples remain in their denominators. |

The authored profile and personality prompt were preserved. This is a preservation
of configuration, not evidence that personality quality has been independently
judged. Existing evaluator/default/review entry points remain available.

## Native verification

Source implementation was verified at `61f777d8711829ad5d92053016405292adb17ad0`.
The affected 24 targets built with Qt 6.8.3 and its matching MinGW 13.1 compiler,
with assertions enabled. Fresh combined CTest verification passed **29/29**
selected checks in 229.02 seconds, including 12 focused checks and 17 existing
runtime regressions. The existing foundation suite, session/identity/isolation,
action audit/cancellation, answer-quality, scoped memory, Studio delegation and
desktop smoke checks passed. The preserved deployed desktop also passed its
`--ui-smoke-test` after installing the matching offscreen Qt plugin.

The four actual registry campaigns passed their automated selections:

| Item | Campaign | Selected checks |
|---|---|---:|
| FND-01 | FND-01-20261007-9e6022c1 | 3 |
| FND-02 | FND-02-20261007-a10c7876 | 2 |
| FND-03 | FND-03-20261007-c7a5e44d | 3 |
| COG-01 | COG-01-20261007-f64c0d8e | 2 |

These campaigns retained real build/JUnit receipts and were not fixture-only
runs. Nonautomated qualification requirements remain pending.

## Actual local-model baseline

Campaign `cognition-61f777d-20261007` used the existing Qwen3.5-4B Q4_K_M weights,
an owned loopback llama.cpp server, 8192 context, the authored profile, the frozen
240-case heldout corpus and seeds 11, 29 and 47. All **720 expected slots** were
recorded: zero missing, unavailable or invalidly bound samples. Independent
evidence review matched all 1,440 source/output references and the retained
forwarded seed requests. It did not supply human semantic or personality verdicts.

| Seed | Strict answer-and-JSON passes | Runs |
|---:|---:|---:|
| 11 | 131 | 240 |
| 29 | 34 | 240 |
| 47 | 19 | 240 |
| Total | **184 (25.56%)** | **720** |

The strict oracle requires both the expected answer and its requested JSON
structure. This score combines answer correctness and format compliance; it is
not a broad factual-accuracy, reasoning or personality grade. Mechanical checks
passed 720/720, while 536 replies failed the strict oracle. All 720 personality
judgments remain unjudged. No model, prompt or profile tuning was performed in
response to heldout outcomes.

The original wrapper exited 2 because `/v1/models` returned different direct
`data[*].created` response timestamps before and after the run. All substantive
provider identities matched. The original campaign receipts remain unchanged,
with `provenanceStable=false` and `liveQualified=false`; they are an exploratory
baseline and are not retroactively qualified.

## Receipt fix and limits

At `c169eb9723594565e48c9307f3dd57cd35a2a90e`, provider identity excludes only the
exact lowercase direct `data[*].created` field. Full raw inventory receipts remain
retained. Model ID, metadata, nested and top-level `created`, properties, loaded
modules, executable, weights and process identities still participate in comparison.
The registered receipt regression passed in 26.18 seconds; timestamp-only drift
was accepted, and genuine identity changes were rejected. Independent source
review found no remaining material issue in that patch.

Fresh actual campaign `provider-identity-smoke-c169eb9-20261007` completed six slots
from two cases across three seeds. Wrapper and runner exited 0 with clean source,
verified build, stable provenance and zero mismatches, while the retained raw
timestamps differed. Its six replies failed the strict answer oracle. This small
run validates the corrected receipt path; it is not a replacement qualification
campaign and is not merged into the 720-run score. Qualification flags remain false.

Independent bound human semantic/personality review and external qualification
receipts remain required. These implementations make future improvements
measurable and auditable; they do not themselves establish improved model accuracy.

Raw local evidence, including failure receipts and JUnit, is preserved outside Git
under `C:/Users/davis/.codex/attachments/revia-foundation-live-2026-10-07`.
The runnable deployed package is retained at
`C:/Users/davis/OneDrive/Documents/GitHub/R.E.V.I.A/build/foundation-61f777d`.
See `FOUNDATION_EVALUATION.md` for commands, ownership and rollback behavior.
