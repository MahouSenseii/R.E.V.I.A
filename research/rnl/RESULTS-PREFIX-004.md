# PREFIX-004: readiness stop, no prefix-reuse comparison

One approved CPU-only attempt stopped at the fixed readiness gate. No cache,
TTFT, prefill or answer-quality comparison was measured. This leaves the prefix
reuse question open; it does not overturn the positive synthetic RNL-003 result.

## What happened

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

## Identities, resource accounting and unknowns

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

## Smallest next decision

First review a small nonmodel readiness-error fixture and bounded diagnostic
capture of the failing stage/error class/status. Preserve secret redaction and
owned endpoint checks. A later live attempt needs a new reviewed request;
nothing retries or raises limits automatically. The current result qualifies
the guard's clean stop, not the prefix-reuse mechanism.

The [frozen protocol](PREFIX-004.md), [read-only identity snapshot](PREFIX-004-identity.json)
and [status](PREFIX-004-status.json) retain their original scope. Preparation
passed 16 mocked existing-helper tests, 15 data-only collector controls and
three Python-only ownership controls; these do not substitute for live results.

## Subsequent model-free diagnosis

[READINESS-005](READINESS-005.md) repaired lost readiness diagnostics and observed
WinError 10013 on a new owned Python server's initial /health request path. That
proves a current socket-access denial, with the responsible policy unknown; it
does not retrospectively establish this attempt's cause. No model retry occurred.

A separately approved permitted-context nonmodel follow-up subsequently passed
the fake server's actual readiness gates. It ran no model and does not
retrospectively identify this PREFIX-004 attempt's underlying cause.
