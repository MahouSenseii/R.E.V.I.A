# Memory retrieval and NCM report follow-up

Curated facts retain weighted reciprocal rank fusion (RRF). An optional local CPU
cross-encoder scores up to 30 fused candidates before the six-memory selection.
The prompt then presents selected facts oldest-first. Explicit owner revision receipts
still identify historical and current facts; a linked correction can accompany an old
retrieved fact. Similarity does not authorize deleting, merging or superseding a memory.

The conversation archive retains its separate database and typed recall policy.
Topic recall and topic-qualified time windows combine lexical and nomic semantic
rankings. All candidates must satisfy the captured participant, companion, audience,
consent and time bounds before ranking. Earliest-mention queries remain chronological.
Ranked topic/window recall selects 12 turns; earliest recall selects four, and plain
or empty-ranked windows select up to 40. Every mode renders into the existing
2,400-character block. The report's Hit@6 statistic is a comparison metric, not
proof of archive answer quality.

`conversation_embeddings` is an additive index. Existing conversations remain intact.
The existing `MemoryAgent` indexes missing vectors in bounded pages, yields to turn
work, retries outages and cancels on subscription retirement or shutdown. Each vector
is tied to a model and dimension. Saving a vector rechecks the captured turn content
and scope because regenerated turns can reuse an ID. Forgetting/pruning a turn removes
its vectors.

Turning chat history off cancels archive indexing immediately while curated memory
indexing continues. Turning it back on, including clearing an off override, follows
the Settings contract and applies after restart. Recovery then resumes under the
current private participant scope.

## CPU reranker setup

Run `Tools/InstallMemoryReranker.ps1` to install the pinned CPU Python environment and
the reference `cross-encoder/ms-marco-MiniLM-L6-v2` safetensors artifact. Setup runs a
reference scoring check. The application never downloads a model at runtime.

The `memoryReranker` settings block contains:

| Field | Default in repository settings |
| --- | --- |
| `enabled` | `true`; older configurations without the block remain disabled |
| `pythonExecutable` | `ThirdParty/MemoryReranker/.venv/Scripts/python.exe` |
| `workerScript` | `Tools/Workers/memory_reranker.py` |
| `modelDirectory` | `Models/MemoryReranker/ms-marco-MiniLM-L6-v2` |
| `cpuThreads` | `4` |
| `timeoutMs` | `1000` |
| `startupTimeoutMs` | `30000` |

Paths resolve from the selected installation, with the existing development ancestor
lookup. Settings take effect at startup. Missing files leave RRF available. The session
owns the worker and shares it across its conversational tiers. Requests accept only
already admitted candidate text; the worker binds an ephemeral loopback port and
requires a per-launch authentication key. It runs offline on CPU.

Busy, unavailable, cancelled, timed-out, malformed and incomplete results preserve
the fused order. A timed-out or cancelled worker is stopped so inference cannot
continue after the request; remaining queries fall back until the next session start.
Queries are limited to 4,096 UTF-8 bytes and each candidate to 8,192 bytes; an
oversized batch falls back without truncating stored facts.

## Reviewing conversational corrections

An explicit private correction such as “I now live in Oslo” can produce a review
suggestion after the ordinary memory worker saves the new fact. The Memory panel
shows the exact old and new texts. Accept marks the old fact historical through the
existing revision receipts, reusing the already-saved replacement. Reject leaves
both facts unchanged. Neither action deletes a memory.

Suggestions are deliberately conservative: an unquoted first-person correction cue,
normal response provenance, current private attribution, the same subject and category,
and overlapping topic words are required. Up to three candidates can be suggested
for a turn. This is a review aid, not a semantic contradiction verdict. Ambiguous
corrections still use the existing manual correction controls.

The session retains at most 32 pending suggestions. They expire on session or scope
retirement. Acceptance rechecks the original digest and latest revision receipt,
the exact saved replacement, and the captured participant, audience and consent.
The proposed replacement is read-only during review; manual editing is a separate
correction. No suggestion changes which fact is current before owner acceptance.

This workflow is implemented; its new worker/session/UI regressions await the final
supported build and test run.

## Optional semantic duplicate marks

The `memoryNli` setting is **disabled by default**, including configurations without
that block. The optional local CPU scorer uses
`cross-encoder/nli-deberta-v3-small@fa2804872c3b4bd748f38c0185cc85775361e735`.
It is a separate entailment model, not the retrieval reranker. No model is downloaded
at runtime, and the scorer starts only during an enabled local session with curated
memory and embeddings enabled. Disabling raw conversation archiving does not disable
curated duplicate checks.

After a durable fact and its vector are saved, the existing memory worker may examine
up to 256 earlier active vector records for the same attributed subject. It considers
at most four candidates with cosine similarity at least 0.80. Existing sensitive-text,
identifier and polarity checks run before inference, followed by conservative checks
for changes in scope, quantifiers, order and attribution. Records in an owner revision
chain are excluded. The optional scorer must return both entailment directions at
least 0.98, both contradiction probabilities at most 0.01, the configured model
identity and valid, untruncated evidence.

An accepted result adds only a row to `memory_duplicate_marks`. Both facts remain
visible to retrieval with the same ranking, and their embeddings and receipt chains
remain unchanged. The transaction rechecks active records, exact captured text,
private subject scope, revision membership and admission before committing. Retired
scope, consent, task or configuration cancels optional work. Missing or malformed
evidence preserves the facts without a mark. Cancellation or timeout stops the owned
scorer; it stays unavailable until another session startup, without a foreground reload.

Owner correction receipts remain the sole authority for current versus historical
facts. A recalled historical record is labeled as such and accompanied by its exact
current replacement; both are presented chronologically. Later corrections make an
old duplicate mark inapplicable. NLI neither chooses the current fact nor supersedes,
merges or deletes a record.

The [72-pair corpus](../Tests/Data/semanticDuplicatePairs.md) preserves the original
24 similarity pairs and their original labels, adds adversarial cases and separates
40 development from 32 held-out cases. Focused SQLite/helper and existing owner
revision tests passed; fake scorers establish control flow, not model quality.
Worker/session integration still requires the rebuilt suite. The feature remains
off; the bounded model-only measurement below does not establish general precision
or live application behavior.

### Measured NLI checkpoint — 2026-10-20

The [offline quality assay](../Tools/Quality/nliDedupBenchmark.md) scored all 72 pairs
through the installed CPU worker functions, using two CPU threads and the unchanged
0.98 entailment / 0.01 contradiction thresholds in both directions. There were no
inference errors. The partition was frozen before inference; thresholds were not
tuned on either split.

| Model-only result | Development | Held-out |
| --- | ---: | ---: |
| Equivalent pairs accepted / total | 6 / 8 | 7 / 8 |
| Distinct pairs incorrectly accepted / total | 1 / 28 | 0 / 16 |
| Uncertain pairs passing the threshold / total | 1 / 4 | 0 / 8 |
| Precision among equivalent/distinct threshold passes | 6 / 7 (85.7%) | 7 / 7 (100%) |

The distinct false positive confused “The user says the assistant likes tea” with
“The assistant likes tea,” removing attribution. The uncertain threshold pass treated
a “long-term astronomy project” as equivalent to an “ongoing astronomy project.”
Neither result authorizes a mark. The attribution pair is separately covered by a
native deterministic-precheck rejection test; the model assay did not invoke that
gate or the scope, shortlist and transaction gates.

After a separate warmup, bidirectional pair scoring averaged 111 ms, with a 135 ms
95th percentile and 199 ms maximum across this small corpus. Model loading took
16.7 seconds and warmup took 1.1 seconds. These are direct worker-function timings,
not application-session latency or resource qualification.

Seven accepted held-out positives are a small denominator, and these manually
authored examples are not a general precision guarantee. The model-only result
does not measure pipeline precision, resolve ambiguous equivalence, or justify
enabling the default-off feature. Raw probabilities, exact file/corpus hashes and
dependency versions are retained by the reproducible assay.

## Measurement boundaries

The supplied report and graph are external experiment claims. Their original scripts,
per-question results, caches and bootstrap files were not supplied. The checked-in
[reconstruction](../Tools/Quality/retrievalBenchmark.md) records its own algorithm,
runtime, model and dataset provenance and emits unrounded per-question metrics.
Its results must be labeled as a reconstruction.

`ReviaAnswerGateLive` disables memory and embeddings. Two runs of that gate can
measure general answer stability but cannot validate retrieval improvements,
cross-session recall or the report's projected answer-accuracy gains. No golden
baseline should be fabricated from the graph.

### Measured retrieval checkpoint — 2026-10-20

The independent reconstruction completed all 1,531 evidence-bearing LoCoMo
questions over 5,882 turns, using the exact nomic GGUF hash from the supplied
report and the pinned local cross-encoder. It used in-process CPU
llama-cpp-python 0.3.35 for embeddings, rather than the report's b11756 server.
These are public raw-turn retrieval results; they do not measure personal
curated facts or final answers.

| Current retrieval policy | Before | After | Change |
| --- | ---: | ---: | ---: |
| Curated Recall@5: cap24 RRF to cap120/top30 reranking | 58.42% | 66.26% | +7.84 points |
| Curated Hit@6 | 67.80% | 74.98% | +7.18 points |
| Archive topic Recall@5: lexical to hybrid plus reranking | 44.73% | 59.90% | +15.17 points |
| Archive topic Hit@12 | 60.61% | 74.46% | +13.85 points |

The curated Recall@5 gain has a paired-question 95% interval of
6.18–9.54 points and a conversation-cluster interval of 6.33–9.34 points.
The supplied graph reports a 7.1-point gain, so its direction and approximate
magnitude are supported, while the absolute values are not exactly reproduced.
Candidate-cap sensitivity matters: the separate MiniLM experiment matches the
graph's RRF value with cap20, which suggests a five-result baseline. The live
six-memory path uses cap24. Full NCM fusion remains statistically tied with RRF
under the cap20 comparison; its apparent advantage changes with the candidate
cap. With reranking, NCM gains another 1.19 Recall@5 points over the implemented
cap120 RRF path; both confidence intervals are positive. Its 1.05-point Hit@6 gain
has intervals crossing zero. That narrower result warrants a separate curated-fact
evaluation before changing fusion. See the
[benchmark methods and results](../Tools/Quality/retrievalBenchmark.md).

Archive measurements use the application's extracted six-term topic, content-only
FTS and 12-turn ranking budget. They do not simulate the 2,400-character rendered
block, so Hit@12 is candidate evidence rather than guaranteed prompt inclusion.

A separate native C++/worker check used 20 public LoCoMo requests selected across
token-load quantiles, with 30 candidates each. All succeeded and matched the
reference ordering within the default one-second deadline: 133–639 ms, mean
403 ms. Worker startup took 7.4 seconds. This bounded sample is not a corpus-wide
latency estimate or a complete application-session test.

A second current-source C++ check compared 40 ordered rankings against the frozen
reconstruction: ten curated requests at caps24/120 and ten archive requests at
caps48/120. All matched, and 40 public/unrelated-participant denial calls returned
no rows. Fixtures loaded 5,207 public turns from nine conversations directly,
bypassing production admission and deduplication. No sampled query exercised a
time window. This supports the sampled ranking port, not full-corpus native,
backfill, prompt-budget or answer equivalence.

## Report recommendation disposition

| Report step | Implementation or boundary |
| --- | --- |
| Establish evaluation | Reproducible retrieval reconstruction and isolated SQLite/prompt regression cases, including reopening a populated store. A twice-run real Main answer baseline remains required before claiming answer gains. The supplied benchmark source files were absent, so this is not a direct port. |
| Rerank top 30 | Optional session-owned CPU worker; retain the requested result budget and bounded fallback. |
| Hybrid archive | Additive archive vectors, existing-worker backfill, scoped hybrid retrieval and reranking. |
| Optional backward lift | Deferred until a separate archive experiment establishes benefit. |
| Retain curated RRF | Preserved, including existing temporal ranking. |
| Supersession and dates | Exact conversational correction suggestions await owner review and use the existing revision receipts on acceptance; new workflow verification is pending. Selected memories render oldest-first. Optional default-off NLI adds advisory duplicate metadata only. |
| Capped-store forgetting | Deferred as recommended; no automatic forgetting added. |
| NCM mood/persona | Not adopted, as recommended; existing identity and affect owners remain. |
| Next-Concept generation | Not integrated, as recommended. |

The report's additional prompt ablation, Expert-model comparison, cache reuse and
q8 context qualification are separate real-model measurements. No model or
personality replacement follows from unmeasured projections.
