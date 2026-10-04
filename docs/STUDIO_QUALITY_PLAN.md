# Revia answer quality and continuity implementation plan

> For agentic workers: use the existing Director/Supervisor/Worker review loop. Each owned task requires a meaningful failing regression, a passing candidate, and independent review before integration.

**Goal:** Improve answers and useful autonomous work while preserving Revia's authored personality, earned identity, and voice.

**Architecture:** Extend existing conversation guidance/review, AgentWorkflow, ActionRuntime, memory, reviewed learning, speech events, and Qt event consumers. No second scheduler, memory store, persona renderer, or tool executor.

**Tech stack:** C++20, Qt 6.8, SQLite, local llama.cpp providers, CMake/Ninja, PowerShell verification.

**Design authority:** The owner's approved seven recommendations and explicit personality-preservation requirement, repository `AGENTS.md`, and the supplied Studio v3 control document. The baseline is main `58f3aef`; historical live7 samples are regression inputs, not proof of current quality.

## Constraints and review focus

- Keep `Config/Profiles/revia.json`, profile baselines, earned identity, relationship state, and authored answer-mode guidance intact.
- Humor, stubbornness, disagreement, annoyance, affection, and personal opinions are allowed. Factual correction must not require a warmer or more agreeable personality.
- Current input and supplied facts take priority over old dialogue; user revisions do not establish a previous assistant error.
- Excluded private context establishes lack of access, not that a conversation never happened.
- Model assertions, HTTP success, and artifact hashes cannot establish task completeness or actual tool execution.
- Keep scoped authority, cancellation, origin admission, hard output filtering, and ordinary reviewed-memory admission.
- Similarity alone never supersedes a memory. Revision needs an exact prior memory ID and explicit authorized input.
- Use disposable verification state. Physical hearing, noise robustness, and subjective personality acceptance stay explicit when unavailable.

## Task 1 — answer guidance, contextual review, and regression corpus

**Owners:** `Agents/conversationStylePolicy`, `conversationAgent`, `conversationQualityMonitor`, existing Core/LLM review adapters, and `Evaluation/conversationEvaluation`.

- [x] Reproduce invented correction history, missed current request, unsupported private-history denial, and requested-length failures with positive personality controls.
- [x] Add concise purpose-specific factual/current-request guidance without changing authored personality or answer modes.
- [x] Supply bounded admitted recent dialogue and captured answer mode to the existing optional review as untrusted evidence. Preserve the off preference and one-review bound.
- [x] Share diagnostic signals between runtime monitoring and held-out evaluation cases; warnings do not prove semantic failure or authorize rewriting.
- [x] Verify raw output remains private until the complete existing admission/filter path passes, and permitted personality survives unchanged.

## Task 2 — task-specific Agent Studio deliverables

**Owners:** `Runtime/agentStudioRuntime` and `Agents/agentWorkflow` contracts and tests.

- [x] Replay incomplete live7-style artifacts with `verified:true` and prove missing requirements block acceptance.
- [x] Require explicit task-owned steps, constraints, risks, success criteria, and prerequisite evidence references as applicable.
- [x] Validate bounded typed structure and exact prerequisite references in native code; retain separate reviewer/parent acceptance and attempt history.
- [x] Verify valid deliverables, malformed output, changed evidence recovery, checkpoint/resume, and budgets.

## Task 3 — actual bounded investigation checks

**Owners:** a Runtime adapter for the existing `InvestigationAgent::CheckExecutor`, then narrow `ConversationRuntime` wiring.

- [x] Reproduce the empty-executor limitation with an approved marker file.
- [x] Admit explicit read/list proposals only through existing captured-stamp `ActionRuntime` operations. Prose is never a command or path extraction rule.
- [x] Distinguish actual observations from interpretations, refused operations, and unsupported check kinds.
- [x] Verify approved-root reads, outside-root denial, cancellation, and stale-origin refusal before and after execution.

## Task 4 — memory provenance and explicit revisions

**Owners:** existing memory storage/reconciliation/recall and memory prompt consumers.

- [x] Pin existing similarity-only preservation behavior and explicit-revision failures.
- [x] Preserve old and revised records with exact provenance links; retrieval identifies historical versus current information.
- [x] Expose source/revision information to existing memory UI without letting widgets decide reconciliation.
- [x] Verify restart, rejected revisions, missing IDs, and unchanged ordinary memory behavior.

## Task 5 — judged quality feedback and reviewed learning

**Owners:** `SelfAssessment`, `LearningRecordStore`, and the existing Learning Studio runtime bridge.

- [x] Record explicit judged failures with criteria, evidence, and dependency fingerprints; aggregate transport success cannot resolve them.
- [x] Deduplicate repeated issues and reuse existing changed-dependency retest rules.
- [x] Allow private learning candidates only through existing review and durable memory receipts.
- [x] Verify failed/unchanged retests, successful held-out criteria, secret exclusion, and pending lessons staying untrusted.

## Task 6 — response and voice responsiveness

**Owners:** existing conversation/speech event correlation and timing owners.

- [x] Distinguish conversation turn IDs from speech utterance IDs and test cancellation/profile switching.
- [x] Measure accepted input to admitted text and first audio where actually correlated; retain existing queue-to-audio measurements.
- [x] Summarize bounded typical and slow-case samples, with unavailable stages explicit.
- [x] Run controlled fixtures and available local-provider comparisons. Document physical microphone/noise/hearing checks separately.

## Task 7 — focused reactive glass presentation

**Owners:** existing Desktop conversation/activity, memory, and learning event consumers.

- [x] Present compact current activity, useful results/evidence, and pending owner actions using existing runtime state.
- [x] Keep advanced detail available without overwhelming conversation; retain the existing glass theme.
- [x] Verify narrow, normal, wide, and 125% layouts with fresh rendered captures.

## Integration and acceptance

- [x] Establish fresh baseline test discovery/execution and preserve profile fingerprints.
- [x] Serialize shared CMake/build/model operations; owned source edits may overlap.
- [x] Run affected native tests, full discovered suite, shipping build, fresh UI checks, and actual local-model quality comparisons when available.
- [x] Independent review checks exact source, personality preservation, privacy/lifecycle, and claims against retained evidence.
- [x] Update architecture/roadmap with actual capabilities and unresolved limits.
- [ ] Integrate accepted updates into main, push to GitHub, verify remote state, and clean only this package's merged branch according to the owner's standing workflow.

Campaign status and detailed evidence live under ignored `build/studio-20261003-answer-quality`. Retest only after changed code, configuration, dependencies, or new evidence; limit each repair to three evidence-driven attempts before reassessing its scope.

## Current integration status (2026-10-03)

| Engineering owner | Status | Current work and evidence |
| --- | --- | --- |
| Director | Delivering | All seven slices connect to existing owners. Shipping and post-repair builds pass; the final full suite is 47/47. Captured action/confirmation and UI approval regressions pass. Main integration, push, remote verification and merged-branch cleanup are next. |
| Answer Quality Supervisor | Frozen and reviewed | Current-task/correction purpose, exact/ceiling/range guidance retained under context fitting, contextual opt-in review, shared native diagnostics/evaluation. Isolated RED/GREEN and clause-negation repair pass. |
| Capability Supervisor | Frozen and reviewed | Typed task deliverables, pinned prerequisites, native investigation read/list execution and attempt-captured improvement reporting. Isolated RED/GREEN pass. |
| Continuity Supervisor | Frozen and reviewed | Exact owner memory revisions, judged quality gaps, reviewed lessons, bounded dependency retests and canceled-write guards. Isolated and integrated runtime checks pass. |
| UI Worker | Frozen and reviewed | Glass status/details, exact answer feedback and selected-memory revision/provenance. Final normal/125% runs pass with 134 captures; 36 representative images reviewed, including repaired narrow fit. Both queued approval paths have actual RED/GREEN, current-private consent and stale-context refusal controls. |

The unchanged authored-profile corpus has six cases and ten turns. Baseline samples
passed 4/6 and 2/6. The initial candidate passed 3/6. The bounded purpose/format
repair passed 4/6 with review off; a separate opt-in reviewer sample passed 3/6
and changed four replies. These limited stochastic samples do not establish an
accuracy improvement. Actual requests and raw responses are retained, including
factual errors, denied scenario revision, unnecessary hostility and reviewer errors.
The earlier compiled model sample passed 4/6, with an invented private passphrase
and an incorrect object-file definition. The latest compiled sample passed 5/6
mechanically but still contains incorrect technical relations, invented history
and omitted requested explanation. The latest actual Session feature run passed
5/8: real tools and reviewed skill learning passed; continuity lost a scenario
name, the reviewer omitted exact prerequisites and the parent did not run, and
a shared reply gave an incorrect object-file definition. Its 17 requests produced
16 HTTP 200 replies and one automatic-memory transport error of unestablished
cause. Earlier 6/8 packets are retained.
The new purpose/format paragraphs are present on the observed wire. Authored
profiles/seeds and all three answer-mode bodies remain unchanged. Existing context
fitting can still compact profile text; this package does not redefine its budget.

The ordinary review preference stays unchanged. Native tests establish composition,
authority, persistence and lifecycle boundaries; phrase checks and HTTP success do
not establish semantic quality or owner personality acceptance. Final acceptance
must report those separately from the build, registered suites and actual Qt renders.
