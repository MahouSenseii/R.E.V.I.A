# Retrieval experiment

`retrievalBenchmark.py` reconstructs the LoCoMo comparison described in the
20 October 2026 NCM report. The report's `benchmark/` directory, original vectors,
raw rankings and significance files were not supplied. This tool is an independent
reconstruction, not a port of those missing files or proof that the original
numbers were reproduced.

It reads one explicitly supplied public LoCoMo file, makes disposable in-memory
SQLite FTS5 indexes, and writes only question IDs, evidence IDs, ranked turn IDs,
metrics and provenance. It does not open Revia's databases, change settings, run
the conversation pipeline, start a server, or download models. Existing curation,
archive admission, subject scope and revision behavior are outside this experiment.

## Inputs and provenance

The reconstruction follows these inspected primary sources:

- [LoCoMo dataset](https://github.com/snap-research/locomo/tree/3eb6f2c585f5e1699204e3c3bdf7adc5c28cb376):
  `data/locomo10.json`, SHA-256
  `79fa87e90f04081343b8c8debecb80a9a6842b76a7aa537dc9fdf651ea698ff4`.
- [NCM loader](https://github.com/iam-saiteja/NCM/blob/98651bb605898f20936c094d9c1169c51f8c00ef/bench/data.py):
  categories 1–4, valid evidence only, speaker prefixes, image captions, session
  order. The resulting dataset has 5,882 turns and 1,531 questions.
- [NCM metrics](https://github.com/iam-saiteja/NCM/blob/98651bb605898f20936c094d9c1169c51f8c00ef/bench/metrics.py),
  [lexical ranking](https://github.com/iam-saiteja/NCM/blob/98651bb605898f20936c094d9c1169c51f8c00ef/ncm/lexical.py)
  and [fusion](https://github.com/iam-saiteja/NCM/blob/98651bb605898f20936c094d9c1169c51f8c00ef/ncm/memory.py):
  BM25, normalized score fusion, backward lift and paired bootstrap.
- Revia's `Private/Memory/hybridRanking.cpp`, `longTermMemory.cpp` and
  `conversationArchive.cpp`: FTS5 tokenizer, quoted prefix terms, curated
  stop words, lexical/semantic weights 1/1.25, rank constant 60, cosine floor
  0.35, bounded candidate lists and newer-first curated ties.

The local nomic GGUF must be the pinned manifest artifact with SHA-256
`d4e388894e09cf3816e8b0896d81d265b55e7a9fff9ab03fe8bf4ef5e11295ac`.
Embedding inputs use `search_document: ` and `search_query: ` respectively.
The tool records the actual file hash. In-process mode also records the installed
llama-cpp-python version and loaded library hash; this is not the report's
llama.cpp b11756 server.

The optional reference reranker is the local
`cross-encoder/ms-marco-MiniLM-L6-v2` safetensors model installed by
`Tools/InstallMemoryReranker.ps1`. That is the canonical model name to which the
report's `ms-marco-MiniLM-L-6-v2` alias resolves. Scoring is direct CPU inference;
it does not use the product HTTP worker or its timeout/fallback behavior.

## Run

Run the deterministic contracts first:

```powershell
python Tools/Quality/retrievalBenchmarkTests.py
```

Download the public dataset separately to an evidence directory, using the pinned
URL above. Keep downloaded data, embedding caches and results out of Git.

The smallest complete experiment needs only Python, NumPy and SQLite FTS5:

```powershell
python Tools/Quality/retrievalBenchmark.py `
  --dataset .revia-ai/evidence/2026-10-20-ncm/locomo10.json `
  --output .revia-ai/evidence/2026-10-20-ncm/locomo-lexical-new `
  --lexical-only
```

To prepare nomic vectors without a server or socket, use an environment with
llama-cpp-python installed:

```powershell
python Tools/Quality/retrievalBenchmark.py `
  --dataset .revia-ai/evidence/2026-10-20-ncm/locomo10.json `
  --output .revia-ai/evidence/2026-10-20-ncm/embedding-preparation-new `
  --cache .revia-ai/evidence/2026-10-20-ncm/nomic-cache `
  --embedding-model Models/nomic-embed-text-v1.5.Q4_K_M.gguf `
  --embedding-inprocess --prepare-embeddings-only --include-archive-policy --threads 4
```

This uses CPU mean pooling with context, batch and ubatch 2,048, rejects overlong
input rather than truncating, and releases the model before returning. Alternatively,
an already running loopback embedding endpoint can be selected with
`--embedding-port`; the tool never launches one.

Once vectors exist, the reranker environment can run the complete comparison
without importing llama-cpp-python:

```powershell
.\ThirdParty\MemoryReranker\.venv\Scripts\python.exe Tools/Quality/retrievalBenchmark.py `
  --dataset .revia-ai/evidence/2026-10-20-ncm/locomo10.json `
  --output .revia-ai/evidence/2026-10-20-ncm/locomo-nomic-new `
  --cache .revia-ai/evidence/2026-10-20-ncm/nomic-cache `
  --embedding-model Models/nomic-embed-text-v1.5.Q4_K_M.gguf `
  --reranker Models/MemoryReranker/ms-marco-MiniLM-L6-v2 --include-archive-policy --threads 4
```

Every output directory must be new. Cache entries bind the exact input text,
prefix and model hash and retain embedding-engine provenance. `--limit` is for
smoke testing only; a limited run does not reproduce the report.

## Interpretation

`queries.jsonl` contains unrounded paired metrics and top-100 IDs.
`summary.json` gives aggregate/category scores and measured latency.
`significance.json` reports paired question bootstrap and conversation-cluster
bootstrap intervals using 10,000 resamples by default. The cluster bootstrap
resamples whole conversations and weights every sampled question equally.
`metadata.json` records hashes, package versions, counts, options and limitations.
A completed run must have `"completed": true`.

Several report details cannot be recovered from the supplied Markdown:

- It says `max(4*k,20)` without identifying `k` used during the benchmark. The
  default reconstructed `revia_rrf` uses `k=10` (40 candidates); the additional
  `revia_rrf_runtime6` uses the actual six-memory budget (24 candidates).
  `revia_rrf_cap20` records the `k=5` sensitivity, and
  `revia_rrf_pool30_rerank` uses the 120-candidate first-stage pool produced by
  asking the production store for a 30-memory rerank shortlist.
- Archive topic terms are not specified. The reconstruction uses Revia's curated
  stop-word tokenizer for the report comparison. Actual archive admission uses
  a separate six-term policy and is not exercised here.
- The report calls the metric MRR@10 while NCM's referenced implementation
  examines the first 100 ranks. Both cutoffs are emitted explicitly.
- The report's reference cross-encoder runtime was a different Linux/PyTorch
  version. Local Windows timing, especially while another build is running,
  cannot be treated as a clean hardware comparison.
- Full runs evaluate the union of the three top-30 shortlists once per query,
  then reuse each candidate's score. This avoids duplicate inference. Per-system
  rerank times include the whole union and are therefore upper bounds on one
  shortlist; `rerank_union_candidates` and `rerank_union_milliseconds` retain the
  actual work. Batching can introduce small floating-point differences from
  separate calls.

The report values embedded in the tool are rounded reference values, not pass/fail
thresholds. A numeric mismatch must remain visible. Retrieval gains do not establish
answer-quality gains, and this tool makes no answer-accuracy projection.
The existing `ReviaAnswerGateLive` explicitly disables memory and embeddings; its
baseline is a separate conversation-quality check.

`--include-archive-policy` adds a separately labeled comparison. It extracts the
current archive's maximum six topic terms, embeds that joined topic string and
uses the same topic string for reranking. The old lexical baseline is
`archive_topic_bm25`; the new hybrid path uses 48 candidates for its 12-result
budget, and its reranker uses the 120-candidate pool that produces a 30-turn
shortlist. These results must not be mixed with `revia_bm25`, which uses the
curated tokenizer selected for the report reconstruction. Recall/Hit@12 are
included alongside the report's @5/@6 metrics. Neither cutoff measures actual
archive prompt delivery: the 2,400-character renderer budget, explicit recall
admission, and current-question removal remain outside the reconstruction.
The archive-policy FTS indexes raw content without a speaker label, as production
does. Embeddings and reranking retain LoCoMo's speaker labels as the public-data
adaptation of `ArchiveEmbeddingText`; the runtime's roles are user/assistant.

## Measured checkpoint: 20 October 2026

PARTIALLY VERIFIED: the complete independent reconstruction finished all 1,531
questions, with `completed: true`, in 1,692.10 seconds after embedding preparation.
The original harness remains unavailable, so the supplied graph was not exactly
reproduced. This is public-data retrieval evidence, not live application recall
or answer-accuracy evidence.

The run used the dataset and nomic model hashes above, CPU mean pooling through
llama-cpp-python 0.3.35, and the reference cross-encoder pinned to
`233902d25c440f23af6f7d6e94d2946bac0bee0a`. The reranker environment used torch
2.7.1+cpu, sentence-transformers 5.1.2 and transformers 4.57.1. The embedding
library SHA-256 was
`25b8aa2dd4befdf561a929fd1df165fba10bcf679ec4bd1bab5806dbb4c5418a`.
Only four inference threads were used. A pre-existing debug build was active
during measurement.

| Ranking configuration | Recall@5 | Hit@6 | Hit@12 |
|---|---:|---:|---:|
| Report-cap sensitivity, budget 5 / cap 20 | 0.586854 | 0.678641 | — |
| Curated reconstruction, current budget 6 / cap 24 | 0.584154 | 0.677988 | — |
| Curated reconstruction, cap 120 then rerank top 30 | 0.662580 | 0.749837 | — |
| Report reconstruction, assumed cap 40 | 0.580279 | 0.673416 | — |
| Report reconstruction, cap 40 then rerank top 30 | 0.676623 | 0.770738 | — |
| Archive policy, old lexical ranking | 0.447307 | 0.514696 | 0.606140 |
| Archive policy, hybrid budget 12 / cap 48 | 0.502125 | 0.589157 | 0.674722 |
| Archive policy, cap 120 then rerank top 30 | 0.598973 | 0.693011 | 0.744611 |

The curated rows apply current ranking/candidate rules to raw public turns, not
to production curated summaries. Archive rows additionally apply the six-term
topic query and content-only lexical index. They must not use the separate
report-reconstruction BM25 baseline (0.505507 Recall@5, 0.574788 Hit@6).
Hit@12 is the archive ranking diagnostic; the renderer's character budget can
still remove evidence before prompt delivery.

All following gains and 95% confidence intervals are **percentage points**.
Question and whole-conversation bootstrap intervals use 10,000 paired resamples;
the cluster estimate retains question weighting and has only 10 conversations.
Intervals are unadjusted per-comparison, and candidate-cap sensitivities are
exploratory.

| Comparison | Metric | Gain (pp) | Question 95% CI | Conversation 95% CI |
|---|---|---:|---:|---:|
| Curated cap 24 → cap 120 + rerank | Recall@5 | +7.843 | [+6.180, +9.537] | [+6.328, +9.342] |
| Curated cap 24 → cap 120 + rerank | Hit@6 | +7.185 | [+5.421, +8.948] | [+5.524, +8.822] |
| Cap 20 → production cap 120 + rerank | Recall@5 | +7.573 | [+5.881, +9.297] | [+5.855, +9.396] |
| Cap 20 → production cap 120 + rerank | Hit@6 | +7.120 | [+5.291, +8.948] | [+5.106, +9.181] |
| Cap 120 RRF + rerank → NCM + rerank | Recall@5 | +1.188 | [+0.211, +2.193] | [+0.087, +2.618] |
| Cap 120 RRF + rerank → NCM + rerank | Hit@6 | +1.045 | [−0.065, +2.221] | [−0.197, +2.675] |
| Report cap 40 → cap 40 + rerank | Recall@5 | +9.634 | [+7.883, +11.362] | [+8.084, +11.079] |
| Report cap 40 → cap 40 + rerank | Hit@6 | +9.732 | [+7.836, +11.692] | [+8.634, +10.807] |
| Archive lexical → hybrid cap 48 | Recall@5 | +5.482 | [+3.481, +7.405] | [+2.815, +7.752] |
| Archive lexical → hybrid cap 48 | Hit@6 | +7.446 | [+5.291, +9.602] | [+4.436, +9.961] |
| Archive lexical → hybrid cap 48 | Hit@12 | +6.858 | [+4.572, +9.144] | [+4.290, +9.567] |
| Archive hybrid cap 48 → cap 120 + rerank | Recall@5 | +9.685 | [+7.913, +11.422] | [+8.064, +11.463] |
| Archive hybrid cap 48 → cap 120 + rerank | Hit@6 | +10.385 | [+8.491, +12.280] | [+8.278, +12.766] |
| Archive hybrid cap 48 → cap 120 + rerank | Hit@12 | +6.989 | [+5.421, +8.493] | [+5.249, +9.120] |
| Archive lexical → cap 120 + rerank | Recall@5 | +15.167 | [+13.123, +17.239] | [+13.620, +16.685] |
| Archive lexical → cap 120 + rerank | Hit@6 | +17.831 | [+15.545, +20.183] | [+15.717, +19.873] |
| Archive lexical → cap 120 + rerank | Hit@12 | +13.847 | [+11.561, +16.133] | [+11.928, +16.211] |

The graph's nomic RRF value was 0.591 Recall@5, versus 0.586854 at cap 20,
0.584154 at cap 24, and 0.580279 at cap 40. No cap exactly reproduced it.
The separate MiniLM check below points to cap 20 as a plausible report setting,
but the missing script prevents confirming that inference. NCM v2 measured
0.584131: its −0.272-point difference from cap-20 RRF is not significant
(question interval [−2.064, +1.526], conversation [−2.213, +2.007]); neither is
its +0.385-point difference from cap-40 RRF (question [−1.288, +2.070],
conversation [−1.188, +2.220]). NCM plus reranking measured 0.674463, versus 0.676623 for
cap-40 Revia RRF plus reranking; its Recall@5 difference is also not significant.
Against current cap-120 Revia RRF plus reranking (0.662580), however, NCM's
Recall@5 advantage is +1.188 points with both intervals above zero, while its
Hit@6 difference has both intervals crossing zero, as tabulated above. Thus this
reconstruction supports a modest cap-dependent Recall@5 gain from NCM after
reranking, without a clear Hit@6 gain. It does not confirm the original graph's
exact scores or establish a production curated-summary benefit.
A useful follow-up is a predeclared comparison of current RRF plus reranking
against NCM plus reranking on held-out curated facts, measuring Hit@6 and
end-to-end answer quality as well as Recall@5. Current RRF remains in place
pending that transfer evidence.

The fusion result depends on the comparison cap. Z-score plus backward lift
measured 0.607581 Recall@5, a +2.730-point gain over cap 40
(question interval [+1.282, +4.185], conversation [+0.716, +4.844]). This
differs from the supplied report's nonsignificant finding. Against cap 20,
however, its +2.073-point gain has question interval [+0.456, +3.718] but
conversation interval [−0.153, +4.449]. Its cap-20 Hit@6 gain is +2.351 points,
with question interval [+0.457, +4.246] and conversation [−0.364, +5.157].
Thus the whole-conversation uncertainty still includes no gain at cap 20.
These source-grounded variants were evaluated without tuning to target scores.
They remain raw-turn, one-dataset comparisons and do not establish a production
curated-summary improvement or justify adopting NCM fusion/lift without separate
validation.

Timing is equally scoped. Full-query reranking scored a union averaging 48.19
candidates (maximum 65), shared by three top-30 lists. Its median was 680.08 ms
and p95 932.07 ms; these are union timings, not single-shortlist timings.
The separate archive top-30 reference pass had median 391.27 ms, p95 534.73 ms,
and maximum 736.16 ms. Those measurements use direct batch-30 reference calls.
The native worker uses batch 16, process/HTTP overhead, startup warmup and a
1,000-ms deadline. **Neither reference timing verifies that native deadline,
timeout fallback, or application response latency.**

A separate current-source native-worker check subsequently passed all 20 public
LoCoMo requests, with all 20 final permutations matching the reference. Ten
requests per curated/archive path were selected across token-load quantiles,
including the shortest and longest padded-token loads; each had 30 candidates.
Measured request times were 132.78–638.68 ms (mean 402.57 ms), under the configured
1,000-ms deadline; startup was 7,405.71 ms and the owned worker shut down.
This verifies the bounded native sample only. It is not a random corpus latency
estimate, a full-corpus timeout pass rate, or an application answer test.
The initial executable launch lacked the MinGW runtime PATH and failed before
application startup; the corrected environment completed successfully. Input,
current-source, executable and model hashes are retained in
`native-locomo-verification.json`, with requests/results in the adjacent
`native-locomo-sample.json` and `native-locomo-results.jsonl`.

The complete local evidence is under
`.revia-ai/evidence/2026-10-20-ncm/locomo-nomic-full/`: immutable runner snapshot,
provenance, unrounded per-query top-100 rankings, category summaries and paired
statistics. `retrieval-comparison.md`, `retrieval-comparison.png` and
`retrieval-latency.json` in its parent directory present those outputs.
The MiniLM **bi-encoder** graph panel is a separate experiment from the MiniLM
**cross-encoder** used here; a nomic run cannot verify that panel.

A separate completed CPU MiniLM bi-encoder run used
`sentence-transformers/all-MiniLM-L6-v2` revision
`1110a243fdf4706b3f48f1d95db1a4f5529b4d41`, unchanged pinned upstream NCM
loader/scorers, empty prefixes, normalized vectors and the upstream float16-cache
then float32-scoring convention. All 5,882 turns and 1,531 questions fit within
its default 256-wordpiece limit. The local result exactly matched pinned
upstream raw Recall@5 for generic RRF (0.462657919) and NCM v2 (0.554769915).
Revia RRF measured 0.434715233 at cap 20, 0.444077306 at cap 24 and 0.469099856
at cap 40. Cap 20 rounds to the supplied Revia bar, 0.435, and NCM rounds to
0.555. The supplied generic-RRF bar says 0.462; nearest-three-decimal rounding
of the verified raw value is 0.463. These checks support the right panel's
relative result while retaining the original-harness uncertainty.

The separate provenance and results are in
`.revia-ai/evidence/2026-10-20-ncm/minilm-reference/RESULTS.md`. Additional nomic
cap comparisons from saved rankings, with no extra inference, are retained in
`cap-sensitivity.json` in the main evidence directory.
