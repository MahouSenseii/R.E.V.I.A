# AI pipeline audit — TASK-REVIA-0066

What was checked, what was actually wrong, how each defect was reproduced before it was
believed, and what is still open.

Eight findings were supplied. Seven reproduced as described. One reproduced in half the
form reported and is recorded that way. Five more were found while tracing them, three of
those in code written during this pass.

Nothing here is closed on the strength of reading the source. Every "reproduced" line is
a run.

---

## The shape of every defect in this pass

All eight were the same kind of mistake: **a check that looked like enforcement and was
not.**

| Where | What stood in for the real thing |
|---|---|
| Automatic memory | the words people use *about* credentials, instead of credential shapes |
| Self-assessment | one vocabulary for writing a category, another for reading it |
| Self-assessment | in-memory state mutated before the write that made it durable |
| Intelligence router | fields that changed the selected tier and that nothing ever set |
| Durable memory | exact text equality, with no notion of the same claim restated |
| Memory classifier | a prompt instruction where a deterministic gate was needed |
| Response filter | generic AI-security vocabulary treated as internal prompt structure |
| Context bounds | an average bytes-per-token used as if it were a bound |

---

## ISSUE 1 — automatic memory stored credentials nobody had labelled

**Before.** `ContainsSensitiveContent` was nine lexical markers: `password`, `passcode`,
`api key`, `secret key`, `access token`, `private key`, `credit card`, `social security`,
`recovery code`.

**Reproduced.** The pre-fix function was extracted from git, compiled standalone and run
over nineteen synthetic credential strings, none of which names what it carries:

```text
LEAK   OpenAI sk-        LEAK   Slack xoxb-       LEAK   bearer
LEAK   OpenAI project    LEAK   Google AIza       LEAK   connection string
LEAK   GitHub ghp_       LEAK   Stripe sk_live_   LEAK   card (spaced)
LEAK   GitHub PAT        LEAK   HuggingFace hf_   LEAK   card (unseparated)
LEAK   AWS AKIA          LEAK   npm_              LEAK   national id
LEAK   AWS ASIA          LEAK   JWT
LEAK   GitLab glpat-     BLOCK  OpenSSH key
```

Eighteen of nineteen passed straight through. The one that blocked did so because its
PEM armour line contains the words "PRIVATE KEY" — still matching English, not a key.

**Root cause.** Those markers are what people write when they *talk about* a credential.
They are absent exactly when someone pastes one.

**Change.** `DetectSensitiveContent` returns a typed `SensitiveFinding` over eight
classes: issuer-prefixed API keys (twenty-eight prefixes, each with a documented minimum
body length and a letters-and-digits requirement), PEM/OpenSSH armour, JWT shape, HTTP
bearer structure, `scheme://user:secret@host`, Luhn-valid payment cards behind a
recognised issuer prefix, US national-id shape with the never-issued ranges excluded, and
the original lexical markers retained.

Entropy is deliberately **not** a signal. UUIDs, SHA-256 digests, commit ids, game asset
names and `0x9E3779B97F4A7C15` all pass, and fifteen such strings assert it. The header
documents what is detected and what is not; recovery codes and unlabelled passwords are
named as gaps rather than guessed at.

**Focused verification.** `Tests/secretDetectionTests.cpp` — 23 blocking cases, 15
passing cases, and a check that a refusal's stated reason never quotes what it refused.

**Live.** The real classifier in front of a real store refused a `ghp_` paste with
*"Potentially sensitive information is never stored automatically."*, carrying none of it
into the reason.

**Remaining limitation.** A password with no marker around it is still an ordinary-looking
string with nothing structural to find. The lexical markers remain the only defence there,
and they are not a complete one.

---

## ISSUE 2 — a restart forgot open self-improvement tasks

**Before.** `Assess` created tasks with categories `conversation_latency`,
`first_audio_latency`, `runtime_reliability`. `Initialize` restored guards by looking for
`performance`, `voice`, `reliability`.

**Reproduced.** A test that lets the engine write its own categories, restarts, and feeds
the same evidence again:

```text
Test failure: A restart created a second task for the already-open category
"conversation_latency". The guard restored on load does not recognise the
category the writer used.
```

**Root cause.** Two vocabularies for one concept, with a translation between them. The
existing suite passed because it hand-wrote history lines in the *loader's* spelling, so
no test ever round-tripped through the writer.

**Change.** The guard holds the category strings the writer wrote, in a set. There is
nothing left to translate. Legacy spellings are mapped once on load, so an older history
still guards the problem it describes.

**Focused verification.** `Tests/selfAssessmentTests.cpp` — every created category
survives a restart without duplicating, and a resolved task stays resolved.

**Changed assertion.** `auditFindingsTests.cpp` asserted `task.category == "performance"`
after loading a legacy record. That assertion was **wrong**: a restored task filed under a
name nothing recognises guards nothing, which is the defect itself. It now asserts the
record is restored under the name the writer uses today. A second assertion counted tasks
under `"voice"` only, so it would have passed vacuously; it now counts both spellings and
requires exactly one.

---

## ISSUE 3 — state mutated before the write that made it durable

**Before.** `ResolveTask` erased the task from `snapshot.openTasks` and *then* called
`PersistResolution`. `Assess` pushed a task into the snapshot and raised its guard before
attempting any write.

**Reproduced.** With the history file made read-only, `ResolveTask` returned `false` and
removed the task anyway. The record saying it was open stayed on disk, so the task
vanished from the panel and returned at the next start with nothing in between explaining
why.

**A third defect found in the same function.** `Assess` appended to `snapshot.conclusion`
*outside* the mutex — a data race against any concurrent `Snapshot()` or `Report()`.

**Change.** `ResolveTask` persists, then erases. `Assess` runs in three phases: decide and
reserve the category under the lock, write outside it so a slow disk cannot block the
runtime event thread, then record under the lock again — releasing the reservation for
anything whose write failed, so a failure cannot suppress the problem permanently.

**Focused verification.** Two failure-injection cases (a read-only file, and a directory
where the file should be) assert the in-memory state matches the disk in both directions.

**Remaining limitation.** The history stays append-only, which was the right architecture
and is unchanged.

---

## ISSUE 4 — routing inputs that changed the tier and that nothing set

**Before.** `RoutingContext` is constructed in exactly two places, both in
`conversationRuntime.cpp`. The full audit:

| field | producer before | consumer | disposition |
|---|---|---|---|
| `visionRequired` | `Generate` | Vision / ExpertVision | live |
| `expertVisionPreferred` | `Generate` | ExpertVision | live |
| `explicitResearch` | `Generate` | Main + Deep | live |
| `recentContextCharacters` | `Generate` | Main | live |
| `previousAssistantTier` | delivery point | Main / Expert | live |
| `previousUncertainty` | **none** | Expert, ExpertVision | **producer added** |
| `suppliedFileCount` | **none** | Expert, ExpertVision | **removed** |
| `toolUseRequested` | **none** | Main | **removed** |

**Root cause, field by field.**

`suppliedFileCount` — this application has no attachment boundary. Chat input is text,
file reads go through the action path and come back as a result rather than as context,
and `/show` puts a picture on a canvas. There was no authoritative origin to connect it
to, so it was removed rather than fabricated.

`toolUseRequested` — a resolved tool or action request never reaches the router.
`ReviaSession::TryHandleCommand` dispatches those turns and returns before any generation
happens, so at routing time the answer is always no. A keyword guess would have been a
different signal wearing the same name.

`previousUncertainty` — given a real producer: what the runtime observed after the
previous reply was delivered (the deterministic filter had to replace it, or the quality
monitor found it ungrounded), plus a failed generation recorded where it returns. Never
the model's own report of how sure it was. This required a new
`responseOutput::bHardFilterBlocked`, because the filter's *block* verdict was being
discarded and only `changed` survived — which is also true of cosmetic repairs.

**Change.** One `BuildRoutingContext(RoutingInputs)` that both call sites use.

**Focused verification.** `Tests/routingProductionTests.cpp` drives that builder from turn
state and follows each condition through to a selected tier. A structured binding over
`RoutingContext` fails to compile if a field is added or removed, so a new routing input
cannot arrive without someone deciding where its value comes from.

**Changed assertion.** A case in `foundationTests.cpp` set `suppliedFileCount = 6` and
asserted Expert. That assertion was **wrong**: it measured a branch no request could
reach. It now routes on the words the request actually contains, and a second case covers
the escalation through `previousUncertainty`, which does have a producer.

---

## ISSUE 5 — semantic duplication (PARTLY FIXED — see the open issues)

**Before.** Deduplication was exact-text equality after case and whitespace
normalisation. That strictness is correct and is unchanged: `"The user likes C++."` and
`"The user does not like C++."` remain two records. The separate problem is paraphrases
accumulating.

**What ships.** A reconciliation stage between exact deduplication and insertion, reusing
the embeddings the search path already maintains. `ClassifyRelation` returns
`Duplicate` / `Refinement` / `Contradiction` / `Unrelated`, and only `Duplicate` changes
what the store does — nothing already written is ever modified or removed.

Three vetoes sit in front of the similarity score, any one of which keeps the records
apart:

- **Polarity** — a difference in negation or change markers makes the pair a
  contradiction whatever the vectors say.
- **Discriminators** — tokens carrying meaning in their exact form (`C++`, `C#`, `.NET`,
  `1.2.3`, `-10`, proper nouns), compared as an *ordered* sequence, so
  `"Alice trusts Bob"` and `"Bob trusts Alice"` do not merge.
- **New content** — a restatement may not reach for a word the stored record lacks.

**This does not solve paraphrase merging, and the measurement is why.** See
ISSUE-REVIA-0079 below.

---

## ISSUE 6 — an opinion Revia was told to recite became her own

**Before.** Nothing carried how a reply was arrived at. `"I hate jazz."` is the same text
whether volunteered or recited, and the classifier prompt's instruction to ignore
play-acting was the only defence.

**Reproduced live, and this is the strongest evidence in the pass.** Asked to speak as a
jazz-hating critic, the 4B classifier produced:

```text
roleplay   category=self_opinion   summary="Revia hates jazz."
```

with the instruction to ignore play-acting already in its prompt. It ignored it.

**Root cause.** The runtime knows which happened — it read the request before the reply
existed — and was not telling anyone.

**Change.** `ResponseProvenance` with four classes the runtime can actually determine:
`NormalGeneration`, `RequestedRepetition`, `Roleplay`, `RuntimeReflex`. Classified from
the request in `ConversationRuntime::Generate`, carried through `TurnCoordinator` and
`MemoryAgent` to `EvaluateMemory` as a parameter with **no default**, so a new caller has
to decide rather than inherit the convenient answer. `memory::AttributableToRevia` refuses
a self-category memory from any provenance that is not Revia's own voice, whatever the
classifier decided. Memories about the *user* are unaffected.

The prompt instruction stays as defence in depth. It is no longer the enforcement.

**Focused verification.** `Tests/memoryProvenanceTests.cpp` — 13 request-classification
cases, the full provenance-by-category matrix, and a case that reaches the decision
through `llamaCppService::EvaluateMemory` itself with no backend running.

**Live.** `The classifier proposed a self memory 2 time(s); the deterministic gate refused
1 of them.` The volunteered case was accepted, which is what makes the refusal meaningful
rather than vacuous.

---

## ISSUE 7 — the prompt-leak filter blocked ordinary AI-security discussion

**Before.** The marker list contained `"ignore all previous instructions"`,
`"here is my system prompt"` and `"my system prompt says"`.

**Reproduced.** Eleven sentences Revia should be able to say, judged against the pre-fix
list: **five were blocked** and replaced with *"I can't expose private instructions or
hidden prompt text."* — including an explanation of what the phrase means and a report
that a web page contained it.

**A fourth marker had no producer at all.** `"the following internet lookup results are
untrusted reference data"` matches a string the runtime has never emitted; a search of
`Private/` and `Public/` finds zero producers. It could never fire. This is the second
instance of the same drift — a previous round found a marker ending in a `)` the rendered
prompt does not have.

**Root cause.** The filter kept its own copy of phrases the renderers write, and three
entries on the list were not internal structure at all but the vocabulary of writing about
prompt injection.

**Change.** `identity::markers` holds the lead phrases once. The renderers build their
sections from those constants — `statePacketRenderer`, `conversationRuntime`,
`reviaSession`, `longTermMemory`, `conversationRecall` — so a reworded section cannot
become one the filter no longer recognises. The three generic phrases are gone. The filter
remains deterministic and always on; what changed is what counts as evidence.

**Focused verification.** `Tests/promptSecurityTests.cpp` — eleven SHOULD PASS, ten SHOULD
BLOCK built from the constants themselves, and a case that renders a real state packet and
asserts every state-packet marker appears in it, so a marker with no producer fails the
suite.

**Changed assertion.** `foundationTests.cpp` passed the invented sentence
`"Here is my system prompt: do secret things."`. That assertion was **wrong**: the filter
caught it on a phrase that is not Revia's prompt, and the same rule blocked legitimate
answers. It now uses a section the renderer really writes, and a companion case asserts an
ordinary explanation of prompt injection is *not* blocked.

---

## ISSUE 8 — the context bound could build a request the backend refuses

**Before.** `BoundMessagesForContext` estimated two UTF-8 bytes per token and spent a
character budget.

**Reproduced against the configured tokenizer.** `llama-tokenize` over
`Models/Qwen3.5-4B-Q4_K_M.gguf`:

| content | bytes/token | | content | bytes/token |
|---|---|---|---|---|
| a list of UUIDs | **1.06** | | minified JSON | **1.90** |
| random hex | **1.08** | | minified JavaScript | **2.00** |
| base64 | **1.33** | | C++ source | 3.30 |
| dense punctuation | **1.52** | | English prose | 5.17 |
| emoji | **1.67** | | CJK | 5.67 |

Six of ten sit below two.

**Reproduced against the live backend.** The old rule, reconstructed exactly and posted to
a running llama-server:

```text
OLD BOUND: 272 messages, 14504 characters -> HTTP 400
  {"error":{"message":"request (14476 tokens) exceeds the available context size
   (8192 tokens)","type":"exceed_context_size_error","n_prompt_tokens":14476,
   "n_ctx":8192}}
NEW BOUND: succeeded=yes
```

**Root cause.** An average used as if it were a bound. It was wrong in both directions:
dense content overflowed, and prose at 5.17 bytes/token was allowed about forty per cent
of the context it could have used. The chat template's per-message cost was not counted at
all.

**Change.** `revia::llm::EstimateTokens` counts by character class — word-like letter runs,
identifier-like runs, anything containing a digit, punctuation, and per codepoint above
ASCII with CJK charged one token and everything else charged by byte. The budget is spent
in estimated tokens, each message charged six tokens of template overhead, and compaction
verifies its own result rather than trusting a ratio, falling back to a tail that fits
rather than returning something over budget.

Neither remedy that was considered proved necessary. No exact-count round trip and no
bounded retry: the estimate itself was the defect, so there is no added latency to measure
and no retry loop to bound. Measured cost of the estimator: **1.4 ms for 63 KB**, once a
turn.

**Focused verification.** `Tests/contextFittingTests.cpp` carries the ten measured token
counts as ground truth and fails if the estimator under-counts any of them, if it
over-counts by more than three times, or if the corpus drifts from the strings that were
tokenized.

**Changed assertion.** `emotionOwnershipTests.cpp` counted characters against 3072 — the
old budget's own arithmetic (usable tokens × 2) restated as a test. That assertion was
**wrong**: a request can satisfy it and still be refused. It now measures the same budget
in the same currency the runtime spends it in.

**Remaining limitation.** The calibration is against one tokenizer. Replacing the model
invalidates it, which the suite states rather than silently re-fitting.

---

## Verification performed

| | |
|---|---|
| Clean build, all targets | from an empty tree, zero errors, zero warnings in new code |
| Native suite | 0 failures |
| CTest | 6/6 |
| GUI smoke | `Revia.DesktopSmoke` passed |
| Live model run | Main on 8080, embeddings on 8081 |
| Intermediate commits | each of the five builds on its own |

Each commit in this task was build-checked from its own staged tree before it landed, so
the history is bisectable rather than only green at the end.

---

## Still open

### ISSUE-REVIA-0079 — paraphrased memories still accumulate

**Severity:** low (records accumulate; nothing is lost). **Status:** CONFIRMED, OPEN.

The stated goal was that `"The user prefers dark themes."`, `"The user likes dark mode."`
and `"The user generally chooses dark interfaces."` stop becoming three records. **They do
not.** What ships recognises a restatement that reaches for no new content word — a vaguer
repetition of something already held — and nothing beyond that.

**Why the threshold is not simply lowered.** Measured against
`nomic-embed-text-v1.5` over twenty-four hand-labelled pairs:

```text
paraphrases        0.801 - 0.983
different claims   0.733 - 0.944
```

The two distributions overlap across almost their whole range. Worked examples from the
same run:

| pair | similarity | actually |
|---|---|---|
| "prefers concise answers" / "prefers **detailed** answers" | 0.944 | opposites |
| "likes dark mode" / "likes **light** mode" | 0.939 | opposites |
| "drinks coffee every morning" / "drinks **tea** every morning" | 0.926 | different |
| "keeps a long-term astronomy project" / "**music** project" | 0.929 | different |
| "prefers dark themes" / "likes dark mode" | 0.898 | the same claim |
| "prefers concise answers" / "likes short replies" | 0.723 | the same claim |

A threshold low enough to catch every paraphrase in the corpus is **0.801**, and at that
setting nine of the twelve different-claim pairs would merge too. Two of them are a
preference and its own opposite, carrying no negation word and no proper noun, so neither
lexical veto would stop them.

**The safe result: similarity alone is not authoritative enough to delete or merge a
durable memory.** An accumulated duplicate is recoverable. A correction deleted because it
resembled the thing it corrects is not.

**Required verification to close.** A labelled corpus large enough to measure a real error
rate, and a comparison method that is not a sentence embedding — an entailment check, or
an explicit contrast/antonym stage in front of the similarity.

### ISSUE-REVIA-0080 — a contradiction is kept but not superseded

**Severity:** low. **Status:** CONFIRMED, OPEN.

When a correction arrives, both records are kept and neither is marked historical. That is
deliberate — deleting the older one is the only irreversible option — but it is not the
temporal truth the task asked for. Recall may surface `"The user likes coffee."` and
`"The user no longer likes coffee."` side by side, in an order decided by ranking.

`ClassifyRelation` already returns `Contradiction`, so the signal exists and nothing
consumes it.

**Required verification to close.** A `superseded_by` column, a migration, and recall and
prompt rendering that can express "used to, now" — then a restart test showing the
superseded record is retained, retrievable, and rendered as past rather than present.

### ISSUE-REVIA-0086 — reconciliation merged a preference with its opposite

**Severity:** high. **Status:** FIXED AND VERIFIED. Recorded here because it was a defect
in code written during this pass, and because the live run found it after the unit suite
was already green.

The first version of the reconciliation stage used similarity plus two lexical vetoes,
with length separating duplicate from refinement. It merged the 0.944 and 0.939 pairs
above. The rule was rebuilt around the new-content veto, which none of the twelve
different-claim pairs satisfies, and the tests that had asserted those merges were
corrected with the measurement quoted in them.

After the repair: `0 of 12 paraphrases merged, 0 of 12 different claims merged.`

### ISSUE-REVIA-0087 — two provenance classes are asserted but not exercised live

**Severity:** low. **Status:** CONFIRMED, OPEN.

`RuntimeReflex` has no producer: a reflex reply short-circuits before
`coordinator.Execute`, so it never reaches memory classification and the class is
currently unreachable. It is kept because the reflex path may change, but it should be
read as unverified rather than working.

`ClassifyRequestedProvenance` also reads only the current request. A roleplay established
several turns earlier and continued without restating it classifies as
`NormalGeneration`. The word list covers the common "stay in character" case; that is not
a solution.

**Required verification to close.** A live multi-turn roleplay where the framing is set
once and the later turns are checked.

### ISSUE-REVIA-0088 — "would rather" is read as a change marker

**Severity:** very low (fails safe). **Status:** CONFIRMED, OPEN.

Measured live: `"The user prefers MinGW over MSVC."` against `"The user would rather use
MinGW than MSVC."` is classified `Contradiction` at 0.983, because `rather` is in the
polarity-marker list. Both records are kept, so nothing is lost — but the relation is
wrong, and a supersession stage built on top of it would be wrong too. Removing `rather`
risks missing "X rather than Y" as a real change, so it stays until there is evidence
either way.

---

## Unverified assumptions

Recorded rather than left implicit:

- Every token measurement used one tokenizer (`Qwen3.5-4B-Q4_K_M`) and every similarity
  measurement one embedding model (`nomic-embed-text-v1.5`). Replacing either invalidates
  the calibration.
- The live classifier results are from a single 4B model on a handful of turns. They
  demonstrate that the deterministic gates are load-bearing; they do not establish a rate.
- The credential detector's issuer-prefix table is bounded by construction and will fall
  behind new vendors. It is a floor, not a guarantee.
- `RuntimeReflex` provenance and multi-turn roleplay framing are untested against a live
  runtime (ISSUE-REVIA-0087).

**Revia's AI pipeline is not complete.** This pass hardened eight specific paths and left
seven findings open.
