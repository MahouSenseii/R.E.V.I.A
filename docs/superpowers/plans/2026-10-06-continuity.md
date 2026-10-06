# Conversation continuity implementation plan

> **For agentic workers:** Execute with superpowers:executing-plans. The owner already approved implementation; no further approval gate is required.

**Goal:** Retain useful admitted dialogue, retrieve memories using its current topic, and fit prompts using the loaded backend's template/tokenizer where available.

**Architecture:** Keep bounded extractive continuity in `conversationContext`, memory query construction in `promptBuilder`, transport/token accounting in `llamaCppService`, and delivered-tier state in `ConversationRuntime`. Restart reconstructs continuity from the existing archive after its owner verifies scope; no new durable memory store is introduced.

**Tech Stack:** C++20, nlohmann JSON, cpp-httplib, existing native fixture checks, Qt-matched MinGW 13.1.

**Spec:** `docs/CAPABILITY_UPGRADE_DESIGN.md`, package 1.

## Global constraints

- Preserve authored personality, relationships, voice, avatar and existing permission/admission boundaries.
- Keep byte-conservative fitting and one bounded backend-overflow recovery.
- Treat extracted dialogue as lower-trust continuity, never an automatically durable fact.
- Do not change context defaults or claim unmeasured 16K/32K quality, latency or resource results.
- No staging or commits from this package; the director integrates verified changes.

## Review focus

- Corrections near the end of a long evicted message must survive, with source order visible.
- A new topic followed by a pronoun must retrieve its recent subject without dragging in an unrelated old topic.
- Missing, malformed, oversized or cancelled tokenizer responses must not create false exact counts or bypass fitting.
- Public and stale turns must not acquire private history; restart must wait for an explicitly compatible archive scope.
- Fallback generation must record the tier actually delivered; missing tier metadata must remain unknown.

### Task 1: Bounded extractive continuity

**Files:** `Public/Core/conversationContext.h`, `Private/Core/conversationContext.cpp`, `Tests/contextFittingTests.cpp`.

**Interface:** Preserve `GetCompressedHistorySummary()`; add `RestoreMessages(const std::vector<conversationMessage>& source, std::size_t recentMessages)` to reconstruct bounded excerpts from caller-admitted archive rows. Stable per-context source sequence accompanies excerpts. This is not a persistence API.

- [ ] Add checks proving late corrections/constraints survive eviction, summary is bounded and UTF-8 valid, and rollback/clear remove the correct state.
- [ ] Observe red in the focused continuity target before implementation.
- [ ] Replace prefix-only snippets with bounded head/tail source excerpts and priority sentence retention for explicit decisions, constraints, corrections and open requests. Keep original speaker and source sequence; newer sources take precedence.
- [ ] Check restart reconstruction produces the same bounded continuity from admitted archive rows while retaining only the requested recent tail.

### Task 2: Contextual memory query

**Files:** `Public/LLM/promptBuilder.h`, `Private/LLM/promptBuilder.cpp`, `Private/LLM/LLamaCPP/llamaCppService.cpp`, `Tests/contextFittingTests.cpp`.

**Interface:** `promptBuilder::BuildRetrievalQuery(const std::vector<conversationMessage>& context)` returns at most 4,000 UTF-8 bytes. The same query feeds embedding and lexical retrieval.

- [ ] Add a failing production prompt test where a pronoun-only follow-up recalls the latest subject; assert unrelated old-topic memories stay out.
- [ ] Implement newest-first bounded recent user/assistant evidence, preserving the latest input and excluding brief social inputs from retrieval as before.
- [ ] Run query, prompt, multilingual and privacy checks.

### Task 3: Backend template/token accounting

**Files:** `Public/LLM/responseTypes.h`, `Private/LLM/LLamaCPP/llamaCppService.cpp`, `Tests/contextFittingTests.cpp`, `Private/Runtime/conversationRuntime.cpp`.

**Interface:** `responseOutput` gains a bounded numeric/string `contextFit` report: accounting method, prompt tokens, output/reserve tokens, input/retained messages, and compaction reason. No text is exposed in diagnostics.

- [ ] Add a controlled HTTP backend with real `/apply-template` and `/tokenize` protocol shapes. Verify a prompt that fits the reported tokens survives intact although its byte bound overflows.
- [ ] Observe failures for unsupported/malformed endpoints, template options, cancellation and overflow recovery.
- [ ] Request template with the same messages/model/thinking settings, tokenize rendered text including special tokens, bound responses and time, and fall back conservatively on failure. Recount a compacted request before declaring its cost measured.
- [ ] Publish cost and compaction diagnostics through the existing runtime component events; preserve current output reservation and overflow retry.

### Task 4: Delivered tier and restart integration hooks

**Files:** `Public/Runtime/conversationRuntime.h`, `Private/Runtime/conversationRuntime.cpp`, `Tests/routingProductionTests.cpp`; session/archive wiring belongs to director.

- [ ] Add red coverage that fallback output metadata, missing metadata and unsuccessful output map to the correct inherited tier.
- [ ] Use actual `responseOutput.selectedTier` only at admitted delivery; preserve public/cancellation exclusions.
- [ ] Supply director exact restart hook using `RestoreMessages` after archive scope verification and bounded row loading.
- [ ] Run focused checks plus the repository suite once director completes the shared build; record all failures and real limits.

## Implementation and verification record

All four implementation tasks are coded. Direct-source core fixtures first failed on lost late corrections and pronoun-only recall, then passed after bounded extractive notes and the shared contextual retrieval query were implemented. A repeated-pronoun fixture exposed the first one-pair lookback as insufficient; scanning a bounded recent topic chain fixed it. A second continuity fixture exposed repeated boilerplate displacing pending work; bounded candidate deduplication fixed that failure.

The full `RunContextFittingTests` suite passed in an independent direct-source linked fixture, including malformed/oversized/cancelled tokenizer responses and one explicit backend-overflow recovery. Delivered-tier helper coverage failed compilation before the helper existed and passed syntax checks after implementation; the director subsequently reported the shared routing production target passed. The initial shared context target had a fixture source-directory definition problem, owned and corrected by the director. The later scoped-memory package changes prompt fixtures to supply explicit participant/companion scope; its integrated rerun remains director-owned.

`RestoreMessages` reconstructs notes from caller-admitted rows; the dependent scoped-memory package supplies durable filtering and session hooks. Token/template accounting is used only by the text chat path, retains the canonical payload when it fits, and falls back to the existing conservative fitter otherwise. Backend protocol fixtures do not establish live model parity, naturalness or latency. Extractive priorities and follow-up cues are English heuristics; UTF-8 preservation is covered, while broad multilingual semantic quality is unmeasured.
