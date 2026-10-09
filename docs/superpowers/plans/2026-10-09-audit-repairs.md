# Revia audit repairs implementation plan

> **For agentic workers:** Use Superpowers debugging, test-driven development and scoped subagent implementation. Root owns integration and the evidence ledger. Do not commit another worker's files.

**Goal:** Repair verified R1–R7/R10 defects, diagnose R11, retain R8 assessment, and evaluate the integrated candidate honestly under R9.

**Architecture:** Preserve the existing policy, conversation, workflow, action and journal owners. Capture workflow context at admission, carry it to native dispatch, and validate it without borrowing unrelated foreground authority. Keep build diagnostics and test inventories explicit.

**Tech stack:** C++20, Qt 6.8.3, MinGW GCC 13.1, CMake/Ninja, Node 24, PowerShell; existing pinned dependencies.

**Spec:** User's 2026-10-09 MODE: IMPLEMENT instruction; attached Revia_Repair_Prompt.txt (SHA256 b5c751694e5e3c96aaa66846782ec2a758b6a3c1273ca529d172ffa5bc8ea62e); independent review at primary checkout build/review-r1-r9-20261009/review-report.txt. Latest user instruction adds R10 CLI packaging and R11 desktop companion-selection diagnosis.

## Global constraints

- Baseline is ad207d6f04aad2a6981fa2a26a8f35124f287c0f; audited base is 44aa6bc4d890fe050a4336c530b340d08a3a0767.
- Preserve permissions, assertions, unrelated user work, models, memories, profiles and voice assets.
- Keep source in existing Public/Private domain owners, tests in Tests, developer tools in Tools.
- One owner per file; CMakeLists.txt and CI belong to build worker. Shared header/test registration requests go through root.
- Reproduce behavioral failures before fixing; retain meaningful negative and success controls and exact command/results.
- No public search of fixture content. Use controlled loopback providers for deterministic integration; label them separately from real-model evaluation.
- Use fresh affected builds. No claim that an old binary proves current source.
- Root creates the reviewable candidate commit and handoff after checks; workers do not stage, commit, merge, push, or delete branches.

## Review focus

- Denials, quotations, latest amendments and long private inputs must not gain external-search authority (Task A).
- Escaped quotes must not acquire or withdraw live format authority; invalid delivery must not enter remembered success (Task A).
- Eviction and restart preserve exact dotted tokens, attribution, corrections and isolation within bounds (Task A).
- Participant/audience/consent changes and unrelated foreground cancellation must not reparent background work (Task B).
- Partial I/O and missing test inventories must not be reported as successful execution (Tasks C/D).

## Task A — Search, structured replies, continuity (R2–R4)

Owner: policy worker. Files: Private/Internet/internetLookupPolicy.cpp, lookupQueryResolver.cpp and matching headers; Private/Agents/replyFormat.cpp; Private/Core/speechAttribution.cpp and conversationContext.cpp with matching headers if needed; Private/Runtime/conversationRuntime.cpp only for lookup/final-request integration; corresponding existing Tests/replyFormatTests.cpp, contextFittingTests.cpp and lookup/runtime fixtures.

- [x] Add retained red cases from the independent probes; run current production owners to record failures.
- [x] Implement clause/quote-aware lookup authority; retain personal locality, genuine freshness and bounded long-query handling.
- [x] Correct scoped JSON negation and escaped-quote masking; preserve genuine withdrawal and latest amendments.
- [x] Preserve dotted tokens in bounded source-linked continuity and durable restoration.
- [x] Verify real runtime lookup callback, actual provider schema, invalid response/history/memory gate, and exact private request after eviction with positive controls.
- [x] Save commands/results and changed-file report under build/repair-evidence/policy; hand off for fresh review.

## Task B — Workflow provenance and desktop diagnosis (R5/R11)

Owner: scope worker. Files: Private/Runtime/sessionTaskContracts.cpp, agentStudioRuntime.cpp, audienceStudio.cpp; Public/Runtime/reviaSession.h and necessary workflow contract types; Private/Actions/actionRuntime.cpp only if needed for contract propagation; Tests/taskContract* and agentTool*; Tests/Fixture/desktopStopTests.cpp. Request shared file changes explicitly.

- [x] Reproduce a real worker proposal-to-dispatch participant/audience switch with native executor/receipt observations; include unchanged and cancelled controls.
- [x] Capture immutable originating host-approved context/contract at workflow admission and carry it into nodes/actions; use real ownership for parent lineage.
- [x] Validate retired scope/consent and cancellation at final native effect, without gaining authority from a newer turn.
- [x] Diagnose R11 with separate expected denial/allowance evidence before choosing production or fixture correction. Preserve original permission assertions.
- [x] Verify scoped receipts, unrelated foreground exclusion, cancellation and desktop companion-switch controls with fresh binaries.
- [x] Save evidence/report under build/repair-evidence/scope; hand off for review.

## Task C — Build, CI, packaging (R1/R7/R10)

Owner: build worker, sole CMakeLists.txt/.github workflow editor. Files: CMakeLists.txt, .github/workflows/build-and-test.yml, focused Tools build/test drivers, Tools/Presence/WebDemo/test/operator.test.js and package scripts if needed.

- [x] Measure current supported compile/link behavior, resource/timing evidence and CLI loader/import failure before selecting changes. Retain all required targets.
- [x] Add bounded concurrency/target structure or other evidence-supported build remedy; preserve logs on failure/cancel with upload time reserved.
- [x] Enforce nonempty expected/discovered/executed CTest inventory and retain JUnit, exact SHA/build/counts/exits/timings.
- [x] Run operator suite in a cheap independent Windows job; force script/config paths containing spaces with credential-output controls.
- [x] Correct CLI-only runtime deployment; verify packaged launch with compiler paths removed and desktop disabled.
- [x] Save evidence/report under build/repair-evidence/build. Coordinate all full builds with root to avoid competing link storms.

## Task D — Journal reading (R6)

Owner: root native implementation. Files: Private/Audit/evidenceJournal.cpp, Public/Audit/evidenceJournal.h only for a narrow existing I/O test seam if required, Tests/evidenceJournalTests.cpp. CMake registration goes to build worker.

- [x] Retain red production mid-read failure test; distinguish missing/empty/EOF/partial I/O.
- [x] Implement checked read through the same production owner, preserve conservative health/intent/receipt semantics.
- [x] Run fresh Windows journal read/scope/repair/crash controls; run Linux where available, otherwise record exact environment gap.
- [x] Save command/results and request independent review.

## Task E — Integration, R8/R9, review and handoff

Owner: root. Files: docs/REVIA_REPAIR_STATUS.md, docs/REVIA_REPAIR_HANDOFF.md, retained regression/evaluation evidence and manifest tooling only as needed.

- [x] Reuse/revalidate old-branch identities/divergence and capability inventory; do not merge/delete it.
- [x] Integrate independent patches, fresh-build all affected/native required targets, run full expected CTest and native relay plus operator checks.
- [x] Freeze candidate source/build and evaluation tasks/settings before collecting paired baseline/candidate results. Include all failed/missing slots and distinguish mechanics/value/semantics/personality.
- [x] Run available real-model evaluation across required families with reproducible manifests; unavailable model/platform/CI gates remain explicit. Do not reuse old scores as current.
- [x] Generate per-task/full diff packages; fresh reviewer checks spec, authority, assertions, source/build and evidence. Resolve supported findings and rerun affected checks.
- [x] Commit the accessible candidate and write handoff with exact base/candidate/worktree, files, R1–R11 status, commands/results and limits. Preserve the candidate worktree for independent review.

## Preflight interface review

| Tasks | Shared boundary | Decision |
| --- | --- | --- |
| A/B | conversationRuntime vs session context | A owns conversationRuntime; B owns ReviaSession/workflow. Coordinate interface changes through root. |
| A/D | shared quote/contract types | D changes journal read only; no parser ownership overlap. |
| B/D | task contracts and durable action audit | Preserve existing TaskContract and journal receipt schema; request type changes before editing shared types. |
| A/B/D with C | test targets/registration | C alone edits CMake; workers send exact registration requests. |
| All/E | builds and evaluation | Root schedules full native builds and freezes the integrated candidate. Workers may run isolated focused builds only. |
| A | tests/implementation | Required red policy/schema/eviction cases map to existing owners and production wire path. |
| B | tests/implementation | Real worker capture and R11 root cause are required before claiming repairs. |
| C | tests/implementation | Required targets and test inventory are retained; packaging launch is independent of installed compiler PATH. |
| D | tests/implementation | Mid-read injection exercises actual checked read owner, not copied stream idiom. |
| E | tests/implementation | Grades depend on fresh relevant paired evidence, not infrastructure success. |

## Verification boundary

Tested code candidate `fd4f0f1f7d554c9851adcb0680ecd15175e3af87` is committed and pushed. Final local and hosted native inventories each passed93/93 with zero skips/failures; native relay, CLI packaging controls, Linux journal and actual Windows operator checks passed. All six hosted jobs are successful in run37988019053, and native artifact11645241309 has verified source identity, JUnit and both Ninja logs. Both paired evaluation rounds remain attributed to their actual58c905de/f20193bc source checkpoints; no production or evaluation inputs changed afterward. All18 grades remain unchanged. The implementation handoff and status record identify all commands, historical failures and capability limits. The final documentation-only child commit changes no tested code. Primary/main and unrelated work remain preserved.
