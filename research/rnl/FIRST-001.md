# FIRST-001: representation equivalence and accounting

Status: **prepared; not run**. Exact ZIP source is unavailable in this checkout.
This protocol extends [the charter](README.md), without executing uploaded code,
reconstructing missing source, or changing any running Revia workload. All measured
fields below remain `null`. This is one CPU toy experiment, not model training.

## Input and execution gate

Before a run, retain the original ZIP through supported Library materialization
with its identity preserved, or use the owner's explicitly confirmed local
original. Keep that provenance privately. The separate HTML remains a static
reference; recovering the ZIP is the required input gate for this C++ experiment.
Public evidence uses sanitized names and SHA-256 digests, never private
identifiers, local paths or transfer URLs.

1. Verify readable complete bytes, archive listing and extraction boundaries;
   refuse traversal, symlinks or unexpected executable payloads. Record original
   and extracted-file digests. No source is inferred from an earlier audit.
2. Statically inspect source, build scripts and fixtures. Identify actual shapes,
   codebook values, index packing/padding, dtype, operation order, state update,
   router tie-breaking and cache predicate. Keep inherited findings separate.
3. Pin source commit, exact reviewed changes needed for instrumentation, existing
   compiler/version/flags, OS/CPU, input sequences and hashes, precision, initial
   state and tolerance. Disable fast-math/reassociation; record contraction/FMA
   settings. If a required existing tool is absent, stop; do not install it.
4. Review the exact execution command and worker/supervisor code before execution;
   obtain any required execution approval. Verify resource controls on that owned
   worker before resuming it. Missing inputs or controls mean `blocked`, not a
   synthetic replacement, pass or measured result.

## Variants and controls

All variants use identical decoded FP32 weights, router/activation rules and
initial state. Compare decoded matrices first. Quantization/retraining is outside
this experiment. No-cache FP32 is the recurrence reference.

| ID | Representation and behavior | Compare with | Status | First divergent step | Max state/output error |
| --- | --- | --- | --- | --- | --- |
| F | FP32 matrix; no state skipping | Reference | not run | null | null |
| U | uint8 codebook indices; no state skipping | F | not run | null | null |
| P | packed codebook indices; no state skipping | F and U | not run | null | null |
| C-exact | U with supplied exact-repeat skip | U, same sequence | not run | null | null |
| C-near | U with supplied near-repeat skip | U, same sequence | not run | null | null |

Describe actual residency: if P's loader expands its indices, label it
`packed on disk / uint8 in RAM`; it cannot support a packed-runtime claim. A true
packed-runtime implementation would need its own reviewed source identity.
If the supplied cache modes cannot be isolated, record the combined mode and
mark the unavailable control explicitly; do not silently change the predicate.

Freeze the following small synthetic corpus after the source review, before
measurement. Let `z` be the verified zero-input vector and `u` its first unit
vector; these are definitions, not claimed recovered fixture values.

| Case | Reset, then run | Purpose |
| --- | --- | --- |
| Z | 16 copies of z | Zero-input skipping and evolving state |
| R | 16 copies of u | Exact repeats with evolving state |
| A | 16 inputs alternating u and -u, followed by u | Changes, return-to-old-input and cache invalidation |
| B | z, u, z; repeat this block four times | Reset-to-zero inputs are not state resets |
| N-below / N-at / N-above | u, then a perturbed u, repeated eight times | Cache threshold boundary and accumulated error |
| Router | A fixed bounded input list chosen by static analysis | Exercise router branches and tie rule; disclose unvisited experts |

For N, use the **actual predicate's metric, dtype and threshold**. Choose
representable perturbations immediately below, equal to and above that threshold
when reachable; record exact vector bits and evaluated distance. If any boundary
is unreachable, record it as such rather than treating it as tested. Do not guess
an absolute-distance metric. Run every applicable case and variant from both zero
state and at least one fixed finite nonzero initial state. Freeze that state's
exact values and bits after source review, before measurement; use identical
initial states across variants and never share evolving state between variants
or cases. Record any genuinely inapplicable case and reason. Reset timed batches
identically. Add no private conversations or evaluation-training examples.

At each step retain input, router scores/tie decision, chosen expert, state before,
state after, output, cache hit/skip decision, and counted work. Compare **every pair**
of F/U/P: experts must agree exactly and every finite state/output component must
have absolute error at most `1e-6`. Nonfinite values or missing steps fail the gate.
Report the earliest divergence plus maximum errors across the whole trajectory;
an equal final output cannot hide earlier drift. Cache controls have their own
trajectory comparison against U. Expected cache divergence rejects that skip as
an equivalent optimization; it does not erase valid no-cache results.

## Before/after accounting

Use the same model boundary for F/U/P, count all experts, and reconcile components
to the whole artifact. Do not compare a complete container with another variant's
matrix alone. Executable and evidence-file sizes are separate.

| Quantity (bytes unless stated) | F baseline | U | P | C-exact | C-near |
| --- | --- | --- | --- | --- | --- |
| Matrix payload / codebook / padding / routing+metadata (each) | null | null | null | null | null |
| Complete serialized model | null | null | null | null | null |
| Resident representation / decoded copy / routing+state / scratch+cache (each) | null | null | null | null | null |
| Empty-worker RSS / full peak RSS / committed bytes (each) | null | null | null | null | null |
| Load time: median, min, max | null | null | null | null | null |
| Per-step time: median, min, max | null | null | null | null | null |
| Matrix MACs / routing / activation / state work (separate counts) | null | null | null | null | null |
| Outcome / exit status / stop reason (each) | not run | not run | not run | not run | not run |

Count serialized bytes exactly. Mark runtime allocator overhead `unknown` if it
cannot be attributed; object-size sums are not RSS. Report full measured process
peak plus the empty-worker baseline separately; their difference is only a rough
incremental estimate, not an exact allocation map. Shared pages and page rounding
can dominate this toy. MAC counts are analytic/instrumented work, not timing.

After equivalence, time one excluded warm-up and five recorded equal-work batches
per variant, in a frozen interleaved order. Use a monotonic high-resolution clock;
record timer frequency/resolution, batch steps and reset policy. Load timing
includes parsing, allocation and unpacking; step timing includes routing, indexing,
activation and state/cache decisions. Keep trace serialization outside that timed
region and disclose that boundary. Preserve raw batch durations. Use the same
repetition count for every variant, fixed by a budget-limited calibration rule
before collecting comparisons. If five batches cannot finish under the common
budget, or timer/noise dominates, record `incomplete` or `inconclusive`; claim no
speedup. Do not clear system caches or alter other processes to stabilize timing.

## Windows resource controls - proposed, not implemented or exercised

Caps for the complete experiment attempt: one CPU compute thread, 256 MiB worker
working set, 600 seconds wall time including warm-up/calibration/all variants,
and **less than** 10 MiB total result bytes including partial logs. No GPU,
training, downloads, servers or dependency installation. Record supervisor costs
separately; these are planning caps, not current measured consumption.

- An existing Python `ctypes` or PowerShell/.NET P/Invoke supervisor can create
  only its new worker suspended, assign an owned Job Object, set active-process
  limit 1 and kill-on-close, and disable breakaway. Resume only after every limit
  succeeds. Nested-job/permission failures stop the attempt; do not weaken limits
  or attach an existing process. [Job Object limit documentation](https://learn.microsoft.com/en-us/windows/win32/api/winnt/ns-winnt-jobobject_basic_limit_information).
- Require a single compute thread and pin the owned worker to one allowed logical
  CPU, recording the mask. Affinity limits execution placement; it does not prove
  the program created only one thread. Review for thread creation and inspect the
  owned worker's thread count; unexpected compute workers stop the run.
  [Affinity documentation](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-setprocessaffinitymask).
- Set and read back a 256 MiB maximum with
  `SetProcessWorkingSetSizeEx` and `QUOTA_LIMITS_HARDWS_MAX_ENABLE`, retaining a
  valid minimum without raising/reserving it. Separately cap committed memory
  through the Job Object as an additional guard. A committed-memory limit is not
  an RSS limit. Any unsupported call stops execution.
  [Hard working-set limits](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-setprocessworkingsetsizeex).
- Query `GetProcessMemoryInfo` for current and peak working set and committed
  memory, retaining the OS high-water mark rather than only sampled maxima.
  Report API errors, limit breaches and cap-induced paging; a hard working-set
  cap can affect latency. [Memory counters](https://learn.microsoft.com/en-us/windows/win32/api/psapi/ns-psapi-process_memory_counters).
- Use a monotonic supervisor deadline, with a small cleanup margin inside 600
  seconds. Terminate only the owned job if time expires. Bound every worker read
  and supervisor write; reserve space for the final stop record before starting,
  refuse the write that would reach 10 MiB, and stop the owned job. A timer/size
  polling loop alone cannot promise exact hard limits; record detection/cleanup
  latency and call overruns stopped/incomplete, never compliant successes.

## Evidence record and decision

One small manifest plus bounded trajectory/batch records is sufficient; no new
framework. Proposed shape (unmeasured fields shown as `null`):

```json
{
  "experiment": "FIRST-001",
  "status": "blocked_inputs",
  "source_commit": null,
  "input_sha256": null,
  "reviewed_command": null,
  "environment_and_limits": null,
  "started_utc": null,
  "finished_utc": null,
  "variants": null,
  "trajectories": null,
  "timing_batches": null,
  "exit_status": null,
  "stop_reason": "Exact archive source unavailable; no execution",
  "conclusion": null
}
```

Each later variant row records its identity/actual runtime representation, cache
mode, sequence digest, byte components and totals, RSS/commit peaks, timing batch
values, counters and outcome. Each trajectory row records variant/case/step,
input and state vectors, router decision, output, skip flag, reference variant,
pairwise errors and first-failure reason. Failed and incomplete controls remain
rows, with unavailable measurements null; never encode not-run as zero or pass.

Pass the no-cache gate only with complete equivalent trajectories and reconciled
accounting. Failure of representation equivalence/accounting stops performance
claims for that variant. Stop on an unmet provenance/review/control gate, resource
breach, dependency requirement, unplanned network/service access or interference
with other work. Preserve partial evidence and the exact stopped phase.

This can establish this toy's equivalence and bounded costs under one setup, or
disprove a specific cache/savings claim. It cannot establish language quality,
Qwen/llama.cpp compatibility, VRAM savings, general speedups, safe training,
frontier capability or release readiness. A useful result remains experimental
until independent review and a separately authorized downstream evaluation;
nothing merges or replaces Revia automatically.
