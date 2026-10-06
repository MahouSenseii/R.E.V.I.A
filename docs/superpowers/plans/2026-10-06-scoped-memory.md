# Scoped memory and restart continuity implementation plan

> **For agentic workers:** Execute with superpowers:executing-plans after the director assigns this dependent package. This document is a plan; it does not claim implementation or validation.

**Goal:** Keep person-specific facts and restored dialogue attached to the participant and disclosure scope that admitted them.

**Architecture:** Extend the existing memory and archive SQLite owners with typed subject/scope values. Carry a captured scope through the existing turn coordinator and memory queue; filter candidates in SQL before ranking or limiting them. Rebuild working continuity from the existing archive only after the current private participant is established.

**Tech Stack:** C++20, SQLite migrations and indexed queries, existing nlohmann JSON revision receipts, native HTTP/session fixtures.

**Spec:** `docs/CAPABILITY_UPGRADE_DESIGN.md`, package 1; depends on `2026-10-06-continuity.md`.

## Global constraints

- Preserve personality, relationship state, consent, companion isolation, captured admission and exact revision receipts.
- Legacy facts and archive rows have unknown attribution. Never infer their owner from the only named relationship, current profile, text label, or present speaker.
- Voice recognition does not authorize private recall. Public, shared and unknown audiences retain their existing private-memory exclusion.
- Use existing databases, workers and archive retention; do not add another memory store or restoration scheduler.
- No ABI/product edits until the director releases the focused-build gate. No staging or commits from the worker.

## Source findings that constrain implementation

- `MemoryAgent::Task` captures admission but has no participant value. Its evaluator runs asynchronously, so reading a current relationship there would misattribute a queued turn.
- `TurnCoordinator::Execute` currently passes text and reply provenance to `MemoryAgent::Submit`. `ConversationRuntime::TurnPolicy` already carries the captured relationship and audience revision.
- `RunTurnUnguarded` archives the request before `ResolveLocalSpeaker`. The private reply receives `CurrentRelationship()`, while the input admission closure still captures the original batch. An introduction must resolve one consistent admitted identity before attribution and archival.
- `CaptureInputContext` invalidates audience history when a participant changes; private `conversationContext` is separate and currently survives that change.
- `DefaultLocalSpeaker()` deliberately returns the anonymous local entity after restart. Startup restoration therefore cannot assume the previous named speaker is present.
- `memories.normalized_summary` is globally unique. Deduplication and IDs must incorporate subject identity; merely adding a participant column would still merge two people's identical preferences.
- Archive restoration, FTS recall, temporal recall and earliest-mention queries are currently unscoped. Filtering after their existing `LIMIT` would hide valid rows behind another participant's candidates.
- Revision digests serialize the corrected category/summary and captured origin. Subject identity must enter new request digests, target comparison and receipts without rewriting historical receipts.

## Review focus

- Two named participants express equal or contradictory preferences in the same second: distinct rows and IDs, no cross-person deduplication or recall.
- An introduction or participant switch occurs while classification is queued: immutable subject plus stale-admission rejection at durable mutation.
- Legacy rows already have revisions, embeddings and coarse unique keys: migrate columns transactionally without relabeling, rewriting receipts or breaking exact replay.
- The newest prior session belongs to a different participant, or a matching session has mixed speakers: SQL returns only compatible rows from the latest compatible session.
- Consent is revoked, audience changes, or shutdown occurs during restoration/recall: no context mutation, prompt publication or durable write from the stale scope.

### Task 1: Typed subject and scope contracts

**Files:** create `Public/Memory/memoryScope.h` and matching `Private/Memory/memoryScope.cpp`; modify `Public/Memory/memoryTypes.h`, `Public/Core/conversationMessage.h`, `Public/Memory/conversationArchive.h`; create `Tests/memoryScopeTests.cpp`.

**Interfaces:**

- `enum class MemorySubjectKind { Unattributed, Participant, Companion };`
- `struct MemorySubject { MemorySubjectKind kind = MemorySubjectKind::Unattributed; std::string entityId; };`
- `struct MemoryScope { std::string participantId; identity::AudienceContext audience; identity::SpeakerSource participantSource = identity::SpeakerSource::Unknown; std::uint64_t consentRevision = 0; std::string companionId; };`
- Add `MemorySubject subject` to `memoryDecision` and `memoryEntry`; add optional `std::string participantId` to `conversationMessage`. These fields are data, never permission grants.
- Add `MemoryScope scope` to `ArchivedTurn`. Empty/default scope remains explicitly unknown.
- Add `bool IsAttributedPrivateScope(const MemoryScope&)` to reject anonymous/unknown IDs, empty audience IDs and non-private audiences for automatic private recall/restoration.

- [ ] Write red checks for known private, anonymous, shared, public and unknown scopes; bounded malformed IDs cannot become attributed subjects.
- [ ] Implement validation with the existing identity constants and enum values. Do not parse rendered prompt labels.
- [ ] Run the new focused target plus audience and speaker continuity checks.

### Task 2: Subject-aware storage, deduplication and revisions

**Files:** `Private/Memory/longTermMemory.cpp`, `Public/Memory/longTermMemory.h`, `Public/Memory/memoryRevision.h`, `Private/Core/memoryManager.cpp`, `Tests/memoryScopeTests.cpp`, `Tests/memoryDedupTests.cpp`, `Tests/memoryRevisionTests.cpp`; selected-row identity in `Desktop/memoryPanel.cpp` and session validation in `Private/Runtime/qualityFeedback.cpp` are director-owned hooks.

**Interfaces:** append `const MemorySubject& subject = {}` to scoped `Search`/`BuildPromptBlock` overloads, keeping explicit owner inventory separate. Add `MemorySubject expectedSubject` to new revision requests; new receipts carry the preserved subject. Existing receipt decoding treats missing subject fields as historical unknown metadata.

- [ ] Write red fixtures for equal summaries from participants A/B, conflicting preferences, same-second distinct IDs, cross-subject correction refusal, unchanged legacy receipts and exact new-request replay.
- [ ] Transactionally add subject-kind/entity columns with unattributed defaults. Use a `v3` dedup key containing kind, entity and normalized summary; preserve legacy keys and only compare legacy rows within unattributed scope.
- [ ] Include subject in new row-ID construction and revision request digests. A correction preserves its exact target's subject; changing attribution is not an implicit side effect of correction.
- [ ] Filter lexical, semantic and temporal candidate SQL by permitted subject before sorting or limiting; filter receipt-linked replacements under the same subject.
- [ ] Render explicit subject metadata. Unattributed personal facts do not become current-speaker facts; exclude them from automatic person-specific recall. Keep explicit owner inventory capable of inspecting legacy rows.
- [ ] Run deduplication, provenance, revision, embedding-backfill and migration checks. Reopen the old-schema fixture twice to prove idempotent migration.

### Task 3: Capture subject across generation, classification and queueing

**Files:** `Public/Agents/turnCoordinator.h`, `Private/Agents/turnCoordinator.cpp`, `Public/Agents/memoryAgent.h`, `Private/Agents/memoryAgent.cpp`, `Public/Core/messageRouter.h`, `Private/Core/messageRouter.cpp`, `Public/LLM/LLamaCPP/llamaCppService.h`, `Private/LLM/LLamaCPP/llamaCppService.cpp`, `Public/Runtime/conversationRuntime.h`, `Private/Runtime/conversationRuntime.cpp`, `Private/LLM/promptBuilder.cpp`; session admission/name resolution wiring in `Private/Runtime/ReviaSession.cpp` and `Private/Runtime/audienceStudio.cpp` belongs to the director.

**Interfaces:** append captured `MemoryScope` to the coordinator and memory submission contract; `MemoryAgent::Task` stores it by value. Pass captured participant identity through the existing classifier call. `promptBuilder` reads the newest admitted typed participant field and uses one subject-filtered query for both lexical and semantic recall.

- [ ] Write red checks that queue A, switch to B, release evaluation, and verify no A result is attributed to B; revoke admission before save and assert no row or event escapes.
- [ ] Bind first-person classification to the supplied participant only when scope permits. The classifier may select speaker/self/unattributed, but may not invent an entity ID. Third-party/ambiguous claims remain unattributed unless an explicit host-resolved subject exists.
- [ ] Set self categories to the companion subject only under the existing reply-provenance gate. Reviewed autonomous findings remain explicitly non-personal or host-attributed.
- [ ] Recheck the existing captured admission inside the store's serialized mutation boundary, including automatic saves, to close a scope-change race between a worker's check and SQLite mutation.
- [ ] Director resolves a typed introduction before deriving the final admitted scope; binds request, response, queue and archive to that same scope; clears private working context and delivered-tier continuity on participant changes. Explicitly preserve the first introduction's established relationship behavior.
- [ ] Run real session fixtures with two participants, cancellation, public denial and queued classification. No provider-supplied ID is accepted as authority.

### Task 4: Scoped archive queries and restart reconstruction

**Files:** `Public/Memory/conversationArchive.h`, `Private/Memory/conversationArchive.cpp`, `Public/Memory/conversationRecall.h`, `Private/Memory/conversationRecall.cpp`, `Public/Runtime/conversationRuntime.h`, `Private/Runtime/conversationRuntime.cpp`, `Tests/memoryScopeTests.cpp`, existing archive/session fixtures; director owns `ReviaSession.cpp`, its header and architecture documentation.

**Interfaces:** add scoped overloads for `Record`, `LoadPreviousSessionTail`, `SearchRange`, `LoadRange` and `SearchEarliest`, each accepting `const MemoryScope&`. Automatic callers must use those overloads. Extend the runtime archive-recall callback to receive the captured scope; retain explicit owner archive-inspection APIs.

- [ ] Write red fixtures with legacy rows, alternating A/B turns, latest nonmatching sessions and revoked consent. Reopen the database as a new process owner and check only matching private A rows reach restoration.
- [ ] Add participant/audience/source/consent columns with unknown defaults. Add an index for scope plus session/turn order; apply scope predicates in both previous-session selection and selected-turn queries.
- [ ] Preserve sensitive-content withholding and session retention. Bound long archived content using head/tail preservation so a late correction survives the archive's 8,000-byte content ceiling.
- [ ] Director defers startup restoration until a compatible named private participant is admitted. Load at most the existing 500-turn session ceiling, recheck admission, then call `context.RestoreMessages(admittedRows, settings.conversation.restoreTurns)`. Zero restore count restores neither notes nor dialogue.
- [ ] Do not compare old audience generation numbers as current authority: use stable participant/audience identity and consent compatibility to select historical evidence, then the current captured admission to authorize publication. Preserve original scope metadata as provenance.
- [ ] Clear restored continuity on participant/audience change; never rebuild from unscoped legacy archive rows automatically. Verify late decisions/corrections survive an actual database close/reopen.
- [ ] Run scoped memory/archive tests, existing privacy/migration/speaker tests and the repository suite. Record any unavailable live-model or naturalness evidence separately.

## Integration and evidence handoff

The director adds focused CMake registrations and session/desktop hooks after reviewing the contract choices above. Native checks must establish attribution, SQL exclusion, migration integrity, queue cancellation and restart behavior. A live corpus with two participants and personality review remains separate evidence; no human-quality or recognition-accuracy claim follows from storage tests.

## Implementation and verification record

The dependent package was released by the director after the first continuity build passed. Implemented contracts use `const MemoryScope* scope = nullptr` on owner-accessible retrieval APIs: null preserves explicit owner inventory, while automatic prompt, classifier, self-inquiry and archive callers supply a captured scope. Empty or invalid supplied scopes deny private recall. Companion identity is required and self-memory uses the actual captured companion ID. Newly host-reviewed findings may carry that explicit companion subject; provenance source strings never grant recall access.

`conversationMessage` carries optional participant and complete captured memory scope. `AddMessage(conversationMessage)` and `RestoreMessages` preserve them. `ConversationRuntime::ResetParticipantContinuity` clears dialogue and delivered-tier carryover. The archive adds `LoadLatestCompatibleTail` to prefer compatible current-session rows when switching back to an earlier participant, otherwise selecting the latest compatible previous session.

Storage retains historical unattributed IDs and `v2` keys. Attributed rows use subject-qualified `v3` keys and IDs; revisions require the exact expected subject. Historical unattributed request digests remain byte compatible. Additive SQLite migrations leave old fact and archive attribution unknown. Archive content truncation preserves a bounded UTF-8 head and tail, and current admission is checked within the write transaction before commit.

Observed verification:

- Red: the new scope test failed compilation because `Memory/memoryScope.h` did not exist.
- Green: direct-source MinGW 13.1 standalone executable passed `RunMemoryScopeTests`, the complete existing `RunMemoryDedupTests`, and `RunMemoryRevisionTests` together. Final run includes different participants with identical same-second summaries, lexical/vector recall, companion self-belief isolation, exact correction/replay/reopen, transactional admission rollback, legacy schema reopened twice, archive close/reopen, late corrections and current-session participant switching.
- An intermediate existing deduplication failure exposed an incompatible change to unattributed row IDs; preserving the original ID/key format corrected it, and the full deduplication suite passed afterward.
- Changed production translation units and queue/classifier fixtures passed standalone syntax checks. `RunScopedMemoryQueueTests` and added real HTTP classifier cases await the director's shared linked test run.
- Changed ranges were formatted with clang-format 22; `git diff --check` passed.

The director owns session admission/restoration, GUI revision metadata, full build and repository-suite results. Fixed backend fixtures establish protocol behavior, not live-model accuracy, latency, personality or recognition quality. No tokenizer-model benchmark or default context-size increase was performed.
### Integration follow-ups

Guarded named-speaker resolution now evaluates captured admission under the registry lock before anonymous identity/evidence/consent migration. Runtime calls it outside audience/speaker locks to preserve registry-before-admission lock order. False/throwing guard regressions and the complete direct-source relationship suite pass.

Accepted Learning Studio lessons bind their subject to the captured companion origin and pass an explicit captured admission through MemoryAgent's durable save and optional queue. Host-supplied Participant subjects remain intact. Legacy rows remain unattributed.

`longTermMemory::LoadScoped(scope, maxEntries=12)` selects matching captured Participant/Companion rows in SQL before a bounded limit. Shared/unknown scope returns nothing. Its new crowding/limit regressions and the complete direct-source scoped memory, deduplication and owner revision suites pass.
