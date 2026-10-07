# Foundation and semantic baseline implementation

Approved scope: FND-01, FND-02, FND-03 and COG-01 from the owner's `REVIA_Complete_Upgrade_Plan.md`, reviewed against main `407c779`. The owner selected implementation with "do them". Other roadmap entries are outside this delivery.

Goal: give existing Revia owners interoperable task/evidence values, durable correlated operation records, reproducible campaigns and independently judged answer-quality accounting. Preserve personality and existing authority.

## Decisions and review focus

- Use `CampaignManifest` consistently; the source plan's `BaselineManifest` is a naming error.
- Evaluation owns `OracleRegistry`, including deterministic checks and explicitly pending human rubrics. Model self-grades cannot supply verdicts.
- TaskContract composes existing RuntimeStamp, MemoryScope, WorkflowBudget and DeliverableContract. It does not mint permission, identity or another budget ledger.
- EvidenceBundle is immutable selected references, never a competing journal/database.
- Durable action intent/result acknowledgements remain synchronous. Runtime telemetry uses a bounded queue with explicit drops; listener exception swallowing cannot authorize actions.
- Journal recovery projects receipts only. Unresolved real effects require independent inspection; never replay an executor during journal replay.
- Campaign identity pins source/build/provider/settings/fixtures/oracles/hardware/seed. Empty test selections and mismatched identities fail explicitly.
- Preserve unavailable/unjudged cases and unique-case/run denominators. Semantic and personality verdicts remain separate.
- The 240-episode, three-run held-out gate is a real model campaign requirement; synthetic aggregation fixtures qualify bookkeeping only. Live reports cannot be fabricated from fixture results.
- Primary checkout has pre-existing IDE/CMake/.output changes; isolate all new implementation. Each implementation worker uses its own worktree/build directory. Director owns combined CMake/session integration.

## Tasks

1. FND-01 contracts supervisor: add Public/Core/taskContract.h, evidenceRef.h, evidenceBundle.h and matching implementation/serialization plus Tests/taskContractTests.cpp. Provide named validation failures, explicit major version rejection and exact stamp/scope preservation. Write 24 independently authored round-trip/invalid fixtures and 100 stale/cross-audience admission variants before implementation. Consumers validate before publication or persistence.
2. FND-02 evaluation supervisor: add Public/Private/Evaluation/campaignManifest, Tools/RunArchitectureEvaluation.ps1, Config/Evaluation/architecture_tests.json and focused C++/PowerShell tests. Validate 12 independent identity mismatches and real CTest inventory/empty/missing selection cases. Export immutable manifests and raw results; nonautomated requirements remain pending.
3. FND-03 persistence supervisor: add Public/Private/Audit/evidenceJournal, adapt existing ActionAuditLogger to the shared persistence owner and retain legacy readable output. Typed Append returns durable/degraded receipt; Read returns scoped core::EvidenceRef. Test 50 records, duplicate IDs, 20 crash points, 100 denied-intent/backpressure attempts and 60 real-effect/result-failure reconciliation cases. Runtime telemetry cannot block durable-intent correctness.
4. Director COG-01: add Public/Private/Evaluation/cognitionEvaluation, focused tests and separate development/calibration/heldout fixtures. Own OracleRegistry and review/output/evidence binding. Extend Tools/Quality/semanticReview and answerQualityLive through existing evaluation owners. Test 30 fluent-wrong/unjudged cases and 720 frozen run slots including cancellation/source mismatch. Preserve existing evaluator/corpus and semantic/personality separation.
5. Director integration: register test IDs and change labels, connect admitted task/evidence and journal runtime adapters, lifecycle/shutdown and surfaced audit health. Keep one session owner and scoped private journal. Build using Qt-matched MinGW 13.1, isolated Ninja output and bounded concurrency.
6. Independent reviewer: inspect ownership, authority, identity serialization, journal crash/error paths, campaign source identity, independent oracle binding and denominator integrity. Address material findings, repeat affected checks and run relevant existing regressions.
7. Actual host campaign and delivery: run the installed local model when possible, record all 240 unique episodes/720 runs and actual independent verdicts. Human/unavailable evidence stays pending; no earned grade or full qualification without required evidence. Preserve reports outside Git, merge verified source to main, push/verify and archive merged worktrees/branches under standing authorization.

## Verification and rollback

Run focused new suites and existing action-audit/runtime events/companion isolation/answer-quality/workflow regressions. The evaluation runner must discover test inventory and use `--no-tests=error`. Validate rollback with retained legacy readers and existing entry points; do not delete user records. Source compilation and fixture tests are distinct from live semantic improvement.

## Actual hierarchy/status

| Actor | Responsibility | Status |
|---|---|---|
| /root Director | Integration, COG-01, acceptance and delivery | All 24 targets built; 29 selected checks passed; four registry campaigns and actual 720-run collection complete; delivery integration pending |
| /root/foundation_contracts | FND-01, native admission, session lifecycle, bound live producer | Core/native/session checks passed; real confirmation and audit regression passed |
| /root/starting_package_audit | FND-02, receipt runner, immutable live evidence and documentation | Runner fixtures and delegated-process regression passed; timestamp receipt fix passed registered check and actual six-slot smoke |
| /root/foundation_journal | FND-03 persistence and crash recovery | Imported; focused durable/crash/backpressure checks passed |
| /root/foundation_independent_review | Source/evidence review | Source and all 1,440 full-campaign source/output references reviewed; final receipt fix accepted; human qualification remains pending |

The initial nine focused CTest checks passed. Independent review drove crash-tail,
oracle, telemetry-lock, campaign-provenance and live artifact fixes. Final review
also required per-sample campaign/evidence admission, one cohort across seeds,
long-input compatibility and a redacted durable audit for refused contracts.
The actual host campaign recorded all 720 slots and passed 184 strict answer-and-JSON
checks. Its original provider timestamp mismatch is retained and leaves that
campaign unqualified. A narrowly scoped timestamp normalization passed the
registered receipt regression and a fresh actual six-slot smoke. No original
campaign receipts or scores were rewritten. Human qualification remains pending.
See `docs/FOUNDATION_COGNITION_ACCEPTANCE.md` for counts and practical limits.

Fresh combined verification passed 29/29 checks, including 12 focused checks and
17 existing runtime regressions (229.02 seconds). A supplied contract retains its
frozen authority subject; host-created legacy contracts capture current policy.
Existing Studio delegation passed after correcting that comparison. Two fixture
setup faults were repaired without changing production liveness or permissions.
