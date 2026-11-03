# PREFIX-004: attempt02 reached the primer deadline

Attempt02 passed real-model authenticated readiness and structural token checks,
then stopped at the unchanged 30-second HTTP deadline during the common seed
primer. No target request or paired cache comparison ran. This is progress in
qualifying the measurement path, not evidence of a cache, latency or quality gain.
The earlier positive synthetic RNL-003 result and historical attempt01 remain.

## Attempt02: completed checks and exact stop

One newly approved invocation used the same pinned Qwen3.5-0.8B Q4_K_M, nine
runtime files, fixed public ORBIT fixtures, settings, schedule and caps. Only
retained readiness diagnostics and the permitted execution context differed from
attempt01. No source or parameter was adjusted during execution and no repeat
followed. The protocol is unchanged; driver commit is
`8a50e7b29800e1a64cc7f478c12e8455eb8b558a`.

Two of 21 planned owned servers launched. Both passed health, wrong-key401
rejection, authorized tokenizer, exact model alias, context2048 and one-slot
checks. Startup plus authenticated readiness took about1.370s and1.274s; these
are not cold-load or first-token timings.

Rendering/tokenization reported438 tokens for seed, shared and changed variants
under both add-special options. Shared LCP was404/438 (92.24%); changed LCP was
4/438 (0.91%). The rendered/token fingerprints matched across both servers.
Those structural gates passed. Backend usage reconciliation did not complete,
so tokenization_choice remains null; structural counts are not emitted usage.

The first planned cell was labelled r1-shared-U, but its failed request was the
common seed primer (cache_prompt=true, max_tokens=1), before the cache-disabled
target. It received HTTP200 with the expected SSE response type, then collected
zero SSE bytes and no visible content, usage, finish reason or timing fields.
The request lasted30.010s before the controller's HTTP deadline terminated the
owned job. Its later connection reset10054 is not proof of an independent server
crash. No target, quality grade or cache comparison exists.

The server log reported prompt processing at85 tokens/9.57s and138 tokens/15.53s.
These partial progress samples establish that processing began; they do not
establish completed prefill throughput, a general CPU fault, or a sufficient new
timeout. No cap was increased to obtain a result.

## Attempt02: costs, cleanup and unknowns

The whole attempt took35.812s including staging, hashing and cleanup. It is not
TTFT or a before/after model-speed comparison. Actual generated-token counts,
cache/evaluated token counts, TTFT, completed prefill/target time, grade, paired
ratios, assessment and performance claim remain null. One primer request was
issued; zero target requests and zero comparison cells completed. The command
tool reported nonzero exit1; owned termination codes were0xE004.

One logical CPU affinity and2GiB private-commit/hard-working-set limits were
verified on both jobs. Maximum partial-processing working set was708,362,240B;
commit was398,336,000B. Controller memory is outside these readings. No memory
saving is established. The unchanged1200s aggregate,45s startup,30s HTTP,
650MiB staging and10MiB evidence caps remained in force.

The ten staged files total557,519,968B and matched the pinned allowlist/hashes
before and after. Seven raw evidence files total78,290B. Both owned processes
exited with no cleanup errors; their exact PIDs were subsequently absent and the
credential directory is empty. Staging and raw/partial evidence remain private.
The [sanitized summary](results/PREFIX-004/summary.json) preserves both attempts,
source hashes, per-run diagnostics, null reasons and path/port/PID-free logs.

The permitted context now demonstrably passes this real model's readiness and
structural-token path. It does not retrospectively prove attempt01's cause or
qualify the unanswered cache/quality question. Candidate for Revia and Integrated
remain None. There was no GPU, training, installation, private conversation,
security-setting change or application integration.

## Next decision

This bounded attempt is concluded. A future calibration of prompt-processing
cost or any changed deadline/fixture needs a separately reviewed protocol; no
second invocation, failed-cell relaunch or parameter adjustment is automatic.
RNL's trainable adaptive/stateful architecture remains the broader research goal.
Cache tests qualify one ingredient, and other workable approaches require their
own evidence. No architecture or training conclusion follows from this timeout.

## Attempt01: separately preserved historical record

The following records the earlier checkpoint before attempt02. Statements about
one attempt/no model retry refer to that historical checkpoint. Its original raw
files and public JSON snapshot are preserved separately and by source hash in
the current summary; no failed result was discarded.

One approved CPU-only attempt stopped at the fixed readiness gate. No cache,
TTFT, prefill or answer-quality comparison was measured. This leaves the prefix
reuse question open; it does not overturn the positive synthetic RNL-003 result.

### What happened

The frozen protocol and collector were independently reviewed before the single
invocation. Source commit `e9e9d832fa382cf2e7ad96477252d84d3e2e4de8` and published
commit `9bafd01090767f3d9ad38dd1b16925619b9ab725` have the identical tree
`555d132076931977a981015e3b67cac85fba7e91`. No source changed during the attempt.

The owned runtime log reports the staged model loaded and loopback listening
at about 1.14 seconds. The controller did not complete its authenticated
readiness checks before the absolute 45-second watchdog. It terminated only the
owned job, confirmed exit, joined its monitor, closed handles and removed its
temporary credential. Cleanup recorded no errors. No retry or limit expansion
followed.

The total failed attempt took 47.297 seconds, including staging and cleanup.
That number is not model loading time, TTFT or a cache measurement. The command
tool reported nonzero exit 1; the supervised child termination code was 0xE004.

The helper catches connection, OS and HTTP exceptions without retaining their
details. The retained evidence cannot identify the underlying readiness cause.
Do not label this a slow-load, GPU, memory, authentication or cache failure.
No fixture-tokenization result, primer, target or authenticated properties
record exists. Tiny authentication/tokenizer probes, if reached within the
readiness helper, were not retained separately; their execution is unknown.

### Identities, resource accounting and unknowns

The Qwen3.5-0.8B Q4_K_M file is 532,517,120 bytes, SHA-256
`bd258782e35f7f458f8aced1adc053e6e92e89bc735ba3be89d38a06121dc517`.
Nine pinned runtime files plus that model total 557,519,968 bytes. The exact
allowlist and all hashes matched before and after the attempt. CPU-only staging
excluded GPU, RPC and alternative CPU plugins. Runtime receipt/source markers
remain consistent identifiers, not independent build attestation; actual loaded
module inventory was not captured.

One logical CPU affinity and 2 GiB process commit/hard-working-set controls were
read back successfully. Peak owned process working set was 677,187,584 bytes;
peak commit was 376,504,320 bytes. These cover startup/readiness, not active
inference or controller memory. No RAM reduction is established. The runtime
log reports one threadpool thread, one slot and context 2048; authenticated
backend properties remain unverified.

The attempt used one of 21 planned server launches and zero measurement cells.
The original 1,200-second aggregate, 45-second startup, 30-second HTTP, 650 MiB
stage and 10 MiB evidence caps were retained. Raw attempt/preflight/log files
occupy 4,197 bytes and remain private and unchanged, with their hashes in the
[public-safe summary](results/PREFIX-004/summary.json). Local paths and the
transient port are replaced in its complete copied log; the credential and PID
are omitted. Staged artifacts are retained privately for review.

Actual fixture token counts, LCP gates, cache/evaluated/generated token counts,
TTFT, prefill and target times, quality grades, paired ratios and performance
claims remain null. No model improvement, application integration, training,
GPU workload, dependency installation, private conversation or model download
was involved. Candidate for Revia and Integrated remain None.

### Smallest next decision

First review a small nonmodel readiness-error fixture and bounded diagnostic
capture of the failing stage/error class/status. Preserve secret redaction and
owned endpoint checks. A later live attempt needs a new reviewed request;
nothing retries or raises limits automatically. The current result qualifies
the guard's clean stop, not the prefix-reuse mechanism.

The [frozen protocol](PREFIX-004.md), [read-only identity snapshot](PREFIX-004-identity.json)
and [status](PREFIX-004-status.json) retain their original scope. Preparation
passed 16 mocked existing-helper tests, 15 data-only collector controls and
three Python-only ownership controls; these do not substitute for live results.

### Subsequent model-free diagnosis

[READINESS-005](READINESS-005.md) repaired lost readiness diagnostics and observed
WinError 10013 on a new owned Python server's initial /health request path. That
proves a current socket-access denial, with the responsible policy unknown; it
does not retrospectively establish this attempt's cause. No model retry occurred.

A separately approved permitted-context nonmodel follow-up subsequently passed
the fake server's actual readiness gates. It ran no model and does not
retrospectively identify this PREFIX-004 attempt's underlying cause.
