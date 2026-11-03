# PREFIX-004: existing prefix reuse observed in one real-model pair

Pair03 completed the approved cache-off/cache-on shared-prefix check on the
pinned Qwen3.5-0.8B Q4_K_M CPU runtime. Cache-on reported399 reused prompt tokens
and39 evaluated tokens; cache-off reported0 reused and438 evaluated. Both targets
reported438 input and26 generated tokens, completed with stop, and returned the
same expected JSON answer: budget_eur7200, sourceD2. Usage and backend counts
reconcile. This qualifies existing llama.cpp reuse on this fixture, not a new RNL
kernel or a reliable speedup. RNL-003's positive synthetic result remains intact.

## Pair03 method and approved change

Exactly one invocation launched two fresh owned servers, U then M. Each passed
authenticated readiness and token gates, processed the identical cache-enabled
one-token seed primer, then the same shared target with max_tokens32. Only the
target cache_prompt flag varied between U=false and M=true. There was no third
preflight server, second pair, failed-cell relaunch or adjustment during the run.

The same model/runtime/fixtures/template/schema and CPU launch flags were pinned.
The per-request HTTP deadline changed from30 to75 seconds; the sampling scope
shrunk from the unfinished21-server study to this one pair. Remaining limits
stayed: one logical CPU,2GiB server commit/hard working set,1200s aggregate,
45s readiness,650MiB staging and10MiB evidence. Four generation requests declared
at most66 output tokens and reported54 actual tokens including both primers.

The private wrapper reused frozen harnessE8 and its helper/resource guard without
changing published source. Fourteen pure mocked controls passed after a reviewer
required deadline checks following potentially slow hash verification; an
independent frozen-source review preceded execution. Full source/config/protocol
hashes and the additive method are retained in the existing summary. The original
[protocol](PREFIX-004.md) describes the earlier full-study scope; it is not silently
rewritten to claim the reduced pair was that full study.

## Pair03 observations, including primer cost

| Measurement | U: cache off | M: cache on |
| --- | ---: | ---: |
| Primer prompt tokens / generated tokens |438 /1 |438 /1 |
| Primer request time |50.589s |50.513s |
| Shared target reused / evaluated tokens |0 /438 |399 /39 |
| Shared target generated tokens |26 |26 |
| Backend target prompt-evaluation time |51.379s |4.647s |
| Backend target generation time |4.598s |4.544s |
| Client target first-content time |51.408s |4.666s |
| Client target complete request time |56.006s |9.211s |
| Target answer / finish |7200,D2 / stop |7200,D2 / stop |

These are two individual observations in fixed order, not estimates with measured
variability. The cached target was warm: its fresh-server primer still cost about
50.5 seconds. Primer-plus-target request costs were106.595s for U and59.724s for M,
excluding readiness/staging/cleanup. The whole172.890-second attempt includes
both arms and those other costs; it is not application TTFT or a paired speedup.
On this run, either fresh primer exceeded the old30-second request budget.
Attempt02's original failure remains separately preserved below.

Rendering/tokenization matched across both servers: seed/shared/changed438,
shared LCP404 and changed LCP4 under both add-special options. Backend usage
reconciled to438; both options have identical token vectors, so selecting false
does not discriminate a unique special-token convention. The observed reuse was
399, not the entire404-token LCP. No internal checkpoint attribution was collected
to explain that five-token gap. The changed variant was only tokenized; no changed-
prefix generation control ran. Both target content checksums also matched.

Requested temperature0, seed42, thinking-off, output caps and cache flags are
client metadata. No whitelisted per-request applied-parameter echo appeared in
SSE, so applied_parameters remains null. Recorded /props values are clearly
labeled server defaults, not independent proof of applied request settings.
Coherent emitted usage/cache/evaluated counts provide the mechanism evidence.

## Costs, cleanup and limits

Both owned resource jobs verified the same one-CPU affinity, memory caps and
ownership controls. U peak working set/commit:772,853,760/461,680,640B; M:
772,636,672/462,782,464B. These readings include model/runtime and exclude the
controller; no memory saving is established. CPU instruction/thread census,
source-to-binary build attestation and whole-machine contention are unmeasured.

The staged ten-file557,519,968-byte allowlist matched its source hashes before
and after. Ten private raw evidence files total137,063B. Both owned PIDs were
confirmed absent after exit; monitors/jobs closed and credentials were removed.
Controller exited0, no cleanup error or extra launch occurred. Attempts01/02 raw
hashes were reverified unchanged; both previous public JSON objects are preserved
in the [three-attempt summary](results/PREFIX-004/summary.json).

One fixed public two-field answer is not language-quality generalization or a
real user workload. The usual4B model, long-context turnover, repeated-run
variability, changed-prefix negative control and application latency remain
unmeasured. This result verifies an existing backend mechanism; it demonstrates
no new RNL architecture, compression, training, adaptive learning or product gain.
Candidate for Revia and Integrated remain None. No GPU, training, installation,
security-setting change, private conversation or application modification ran.

## Next decision

This approved pair is concluded. Preserve the positive real-model mechanism
evidence alongside the synthetic RNL-003 result and earlier failures. The smallest
later qualification would add a changed-prefix negative control and repeated
balanced shared-prefix measurements only if that answers a concrete Revia need;
no further model run is authorized by this outcome. Existing prefix reuse is a
workable ingredient, not proof that a new cache should be added. RNL's broader
trainable adaptive/stateful goal still needs separate architecture and held-out
quality evidence; cache work does not define that whole research program.

---

## Historical attempt02 and attempt01 records

The following report is retained from the prior checkpoint. Its next-decision
and no-subsequent-invocation statements describe that checkpoint, before the
separately approved pair03. No earlier failure or null metric was replaced.

### Attempt02 (historical): reached the primer deadline

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
