# RNL-002 results

**Measured, toy numeric contract only.** A one-entry cache of the raw selected
expert's matrix product preserved all 280 tested numeric steps bit-for-bit.
Omitting the expert from the key failed the predeclared negative control. No
latency, process-memory benefit or Revia language-model improvement was tested.
There is no Candidate for Revia or Integrated promotion.

The [evidence summary](results/RNL-002/summary.json) records the measured results.
The [protocol](RNL-002.md) was committed as `b8dfac36` before execution, with
20 frozen trajectories and 280 steps. The sample model, original source and
[FIRST-001 results](RESULTS-FIRST-001.md) were preserved; this is a new contract
experiment, not a silent change to FIRST-001's failing cache.

## Contract and checks

U recomputed every matrix product. M cached one raw FP32 product, keyed by the
selected expert index and all three input FP32 bit patterns. B deliberately
omitted the expert index. All modes ran routing on every invocation, then
activation and state updates. Cache entries reset for each case/state seed;
activation operated on a value copy and never overwrote the cached raw sums.
The model remained immutable for the whole worker lifetime.

The independent checker passed **seven tests**. It checked the uncached reference
against the independent FP32 oracle at the declared `1e-6` tolerance. M then
matched U's selected experts, scores, raw products, outputs and before/after
states **bit-for-bit**, with zero observed state/output error across all 280
steps. The signed-zero cases produced the required
miss/hit/miss/hit/miss/hit pattern for each state seed, conservatively missing
when an input changed between positive and negative zero.

The worker used the fixed MSVC `/fp:strict` configuration. Every worker verified
round-to-nearest and unchanged SSE rounding/FTZ/DAZ control
settings. FTZ and DAZ were both disabled. Floating-point exception status flags
were recorded but explicitly outside the numeric contract; unchanged control
settings do not mean that all exception flags or other side effects matched.

## Counted work and rejected control

| Mode | Cache hits | Cache misses | Matrix MACs | Numeric outcome |
| --- | ---: | ---: | ---: | --- |
| U: no cache | 0 | 0 (no lookups) | 972 | Reference |
| M: expert plus exact input bits | 77 | 203 | 834 | Matched U |
| B: input bits only | 102 | 178 | 684 | Rejected |

M avoided **138 matrix MACs** on this corpus. It still performed 7,560 router
products, 840 activations and 840 state updates, equal to U. It also performed
280 cache lookups, 437 input-bit equality comparisons and 260 expert-index
comparisons. These are source-level operation counts, not CPU instruction
counts or evidence of a speedup. No comparative timing was run.

B produced 25 state-divergent steps, with maximum absolute state error
`0.092685282` and output error `0.167425096`. On the second `[0,1,1]` input in
`RouteRepeat` from zero state, routing changed from coding (expert 1) to vision
(expert 2). U and M recomputed the required product `[1,1,1]`; B incorrectly hit
and reused `[1,1,2]`. Its third state component became `0.675519824`, versus
`0.60017854` for U/M. More hits and fewer counted MACs did not make that control
correct.

## Capacity, provenance and limits

The enabled M/B entry occupied 32 bytes by `sizeof`: 12 input-key bytes, 12 raw
product bytes, a 4-byte expert index, one validity byte and three padding bytes.
U enabled no cache entry. VM object sizes were 64 bytes for M/B and 40 bytes for
U. These figures exclude referenced model allocations and other runtime costs;
full VM allocation and process-memory benefit remain unestablished.

The worker source SHA-256 was
`8163fb3ffa478ed93557df758920a9f8fae0eef26e6a242c14ef38d36d767984`;
the checker SHA-256 was
`3318bf8c1dd416d1ed21cc25a29854c148c6b7962994bfd47f0eebff630f274e`.
Three supervised workers completed in a 1.281-second attempt. The attempt
artifacts occupied 617,060 bytes before the 12,390-byte derived summary, or
629,450 bytes including it. All three exited 0 with resource limits verified.
The build retained six inherited size-conversion warnings from the original
source.

Controls bounded the owned workers to one logical CPU, 256 MiB working set and
committed memory, deadlines and reviewed output caps. Affinity does not prove
one OS thread, and the resource supervisor is not a network/filesystem/GPU
security sandbox. The attempt's elapsed time is bookkeeping, not a cached-versus-
uncached latency result. No training, GPU workload, model change, dependency
installation or unrelated Revia work was part of the experiment.

The evidence supports this immutable, top-1, FP32 toy under its fixed operation
order and FP environment. It is not a general proof for other models, reloads,
approximate keys, exception flags or runtime settings. Cross-model reuse would
need invalidation or a model identity in the key. A representative size or
throughput comparison could be proposed separately; this bounded experiment
stops here, without Qwen/llama.cpp, memory-saving or speed claims.
