# PREFIX-004: real-model prompt-prefix reuse qualification

Status: LOCAL PREPARATION, awaiting parent review of this exact protocol and
implementation before any selected llama.cpp binary/server/model execution.
Publication is pending.
No performance measurement has run. The earlier positive RNL-003 result stands;
this is its reviewed alternative lead, not another toy matrix timing variant.

## Question and fixed scope

On the verified Qwen3.5-0.8B Q4_K_M artifact, does existing llama.cpp active-slot
prefix reuse reduce evaluated prompt work and target first-content latency for a
fixed public shared prefix, with an early-changed-prefix control? Use the numeric
fields Revia already preserves, not private conversations or new telemetry.
This directly qualifies an owned backend; it does not measure the installed
Revia application, the 9B Main model, language ability, training or an RNL kernel.

Reuse selected helpers and public ORBIT facts from
`Tools/Quality/contextBenchmark.py` (raw SHA-256
`914b87e3fbb2e9fdaec751d7ebbb996b6903bf1db35861c9b11fc4aa655bb52c`).
Its original GPU CLI/run_context is unsuitable: do not call it, GpuSampler,
gpu_snapshot, nvidia-smi, or adaptive fit_probe. The research adapter supplies
only CPU arguments, fixed fixtures, bounded streaming and owned-process control.
Reuse the unchanged `run_bounded.py` API types/Windows limit machinery (SHA-256
`31edb4ff8a997363d07ea8823a2efb7bdde5ed2566872a9e6adbda20b5b62bb7`);
its original 256MiB limit cannot load this model, so a reviewed experiment-local
owned-server adapter must explicitly enforce the larger caps below.

## Verified identities and staging gate

Readonly verification found the 532,517,120-byte model, SHA-256
`bd258782e35f7f458f8aced1adc053e6e92e89bc735ba3be89d38a06121dc517`,
matching Revia's manifest. GGUF v3 identifies qwen35, 24 blocks, embedding length
1024 and full-attention interval 4. Tokenizer and template fingerprints are in
[the public-safe identity record](PREFIX-004-identity.json). The private manifest
`.verification/prefix004-identities.json` retains exact source paths and hashes.
The unverified 9B file has a different size from its repository manifest and is
outside this experiment; do not load it or infer its identity.

The installed runtime receipt says b10453/CUDA; embedded markers are consistent
with audited source `3cb7ffb1a1f612d5e4a46244ae5a3c77ad934a70`. Most PE version
resources are empty. These facts do not independently attest a build or establish
which dynamic CPU plugin would load in the installed directory.

Before FUTURE execution, create a fresh task-owned staging directory with only
the nine pinned runtime files named in the identity record and one model copy.
Rehash sources and copies; reject additions, symlinks or mismatches. The nine
runtime files total 25,002,848 bytes; model plus runtime is 557,519,968 bytes.
Cap staging at 650MiB, separately from the 10MiB evidence budget. Never modify the
installed directory, copy CUDA/RPC/other CPU plugins, install CRT dependencies,
or use that installed directory as the server working/backend search directory.
Sanitize backend/environment overrides and use the staged generic CPU-x64 plugin.
If the CPU-only dependency closure or required flags cannot be established, stop.
Do not fall back to the installed CUDA runtime or query/list GPU devices.
The pinned runtime reads config.ini before CLI parsing. Point ONLY the future
child's PROGRAMDATA, APPDATA and LOCALAPPDATA at fresh empty task-owned roots;
verify their llama.cpp/config.ini files are absent. This excludes automatic
private LoRA/RPC/template configuration without reading or changing user config.
Do not repurpose HOME/CODEX_HOME. Windows config/cache mapping is documented in
[pinned common.cpp](https://github.com/ggml-org/llama.cpp/blob/3cb7ffb1a1f612d5e4a46244ae5a3c77ad934a70/common/common.cpp#L999-L1040).

## Frozen public fixture and controls

`prefix004-fixtures.json` contains three message variants: seed, shared probe,
and early-changed probe. Reuse ORBIT's original facts and correction, plus two
existing public inventory distractor turns. The small requested JSON contains
`budget_eur` and `source`; expected values are 7200 and D2. The schema constrains
types and permitted source IDs, not the expected answer. A correct answer remains
a small fixture check, not a model-quality qualification.

Seed and shared probe use the same ordered, labelled system rules. Their final
question wording differs. The changed probe swaps the first two complete rule
paragraphs; facts, requested answer and remaining messages stay identical.
Changing an early rule order is the control, not selecting a friendlier corpus.
No real user conversation, profile, memory database, attachment or runtime log.

Exact token lengths are currently UNKNOWN: no server/tokenizer was run. During
the separately approved preflight, render/tokenize only these frozen variants
using /apply-template and /tokenize with explicit special-token/template options.
Require identical shared/changed probe token counts, 256..1024 prompt tokens,
and adequate context reserve. The seed/probe token LCP must be >=75% of the
shared probe length for shared, <=25% for changed, with a clear contrast.
Chat-template/BOS tokens can remain common in the changed case.
Reconcile reported usage.prompt_tokens with the exact rendered/tokenized count.
If counts, LCPs, template identity or semantics do not match, preserve the records
and stop without editing fixtures, adaptive fitting or a timing retry.

Use one untimed owned preflight server, then 20 fresh owned measurement servers:
five repetitions of shared/changed x cache off/on. Each fresh measurement server
receives the SAME one-token seed primer with cache_prompt=true, then one target.
Fresh processes prevent cross-cell slot contamination and avoid assuming that
cache_prompt=false erases slot storage. Primer/startup costs are recorded and
excluded from target TTFT; total preparation cost remains visible.
Disable built-in startup warmup with --no-warmup on every server; the fixed
primer is the explicit inference warmup. The 660-token budget below covers HTTP
declared output, not input prefill computation. Keep the selected binary's
default checkpoint policy: message-boundary/near-end checkpoints can bypass the
8192-token spacing default, so short prefixes are eligible in principle.
Rendered input LCP is not a guaranteed effective recurrent reuse fraction.

Frozen order, U = cache_prompt:false, M = cache_prompt:true:

| Repetitions | Four cells in execution order |
| --- | --- |
| 1, 3, 5 | shared U, shared M, changed M, changed U |
| 2, 4 | changed U, changed M, shared M, shared U |

All cells use the same GGUF/template/adapter-free identity, settings, seed42,
temperature0, tools absent, thinking/reasoning off, stream=true and
stream_options.include_usage=true. Primer max_tokens=1, target max_tokens=32:
at most 660 declared output tokens across 40 generation requests. Stop rather
than raise output limits. Render/token/template drift between cells invalidates
the comparison. No optimization, parameter search or extra repetition follows.

## CPU launch and budgets

The proposed explicit server arguments are:

```text
--model <task-owned pinned model> --alias rnl-prefix004-0.8b
--host 127.0.0.1 --port <fresh owned port> --api-key-file <owned temporary file>
--device none --gpu-layers 0 --split-mode none --threads 1 --threads-batch 1
--ctx-size 2048 --parallel 1 --batch-size 128 --ubatch-size 128
--cache-type-k f16 --cache-type-v f16 --flash-attn off --cache-ram 0
--no-context-shift --reasoning off --fit off --offline --no-slots --no-warmup
```

`--cache-ram 0` disables the separate RAM prompt cache, not active-slot reuse.
Current source is audited; supported launch behavior remains unmeasured.
Use no shared Revia server or fixed existing port. Pick a fresh loopback port,
require our child still alive, wrong-key rejection, our ephemeral-key acceptance,
the exact alias/context/one-slot properties, and fail rather than attach to an
unrelated endpoint. Keep /slots disabled; it exposes prompt text.

Each server starts suspended, joins an owned Windows Job before resuming, and
must pass limit readbacks: one allowed logical CPU, one process/no breakaway,
2GiB private commit and hard working-set maximum, kill-on-close. No weaker
fallback. Set only task-owned TEMP/TMP and thread controls, inheriting required
Windows system paths while excluding backend/user environment overrides.
Affinity does not prove one OS thread; hard working-set controls can affect time.

Aggregate cap 1,200 seconds, including staging, startup, preflight, primers,
requests and cleanup; cap each startup at 45 seconds and HTTP operation at
30 seconds, clipped to the remaining deadline. Independent monitoring must stop
the owned job even if an HTTP read stalls. These are bounds, not completion
estimates. Stop if the caps prevent a valid study; do not extend them automatically.
Per-launch model/runtime hash verification warms filesystem cache and is excluded
from the startup timer, while remaining included in aggregate attempt time.
Do not describe startup or loading as cold-machine measurements. Staging I/O
has before/after deadline checks; an OS-delayed operation can overrun and must
leave the attempt incomplete, with no later model launch after the deadline.

Limit each generation stream to 64KiB and one SSE line to 16KiB, bounded/redacted
server log to 64KiB per process, and all retained evidence to <10MiB. Bound JSON
preflight responses and derived results separately; preserve partial/truncation
status on excess. Never print/store the ephemeral API key in public artifacts.
Use a fresh task-owned private temporary directory; default sandbox TEMP was not
writable during nonmodel helper validation. Do not alter its permissions.

Finally terminate ONLY the owned job/process, confirm exit, close handles and
remove only our temporary credential. Keep the staged artifacts and partial
results for review. Do not inspect/stop other workloads, clear caches, change
Revia settings, install dependencies or start GPU/training work.

## Measurements, unknowns and interpretation

Parse every SSE JSON object, including the last chunk with choices:[]. Require
complete stream termination and retain finish reason, actual generated count,
fixture grade and response checksum. Timing starts immediately before sending
the target request. TTFT ends at first NONEMPTY content delta; headers, role-only
chunks, null/empty content and usage are not tokens. Missing TTFT remains null.

Preserve Revia's existing units: cache_n/prompt_n/predicted_n are token counts;
prompt_ms/predicted_ms are milliseconds. Retain usage and exact prompt lengths,
client target wall time, separately measured primer/startup time, and all five
paired raw results. Record the client's timer resolution and empirical minimum
positive perf_counter read gap without treating it as CPU frequency.

Counts needed to establish the mechanism must be present and consistent. If
mandatory cache_n/prompt_n/usage fields are absent, keep null and stop as
unsupported; never substitute zero. Missing optional time/memory fields remain
null with a reason. Disabled reuse must report cache_n=0; changed prefixes need
not. Qwen recurrent checkpoint fallback may legitimately yield no shared reuse:
that is a valid negative result, not permission to retune the fixture.

Report target TTFT and prefill min/median/max and each paired U/M ratio. Claim a
local latency benefit only with correct complete fixture answers in every target,
consistent prompt counts, demonstrated shared reuse, adequate timing resolution,
median paired TTFT U/M >=1.10, every pair >1.05 and max(M)<min(U). Use >=10,000
empirical timer quanta for compared TTFT. These five pairs are descriptive, not
population significance or a Revia/9B deployment guarantee. Missing/negative or
overlapping results remain unproven. Treat changed-prefix timing as a control,
not an assumed zero-work or zero-hit baseline.
If a changed-prefix control reports no additional reuse yet passes the same
latency-benefit rule, flag possible confounding and withhold the shared-case
causal speed claim. Preserve the measurements; do not rerun for a desired result.

Keep generated-token counts and finish/output checks visible. If decoded lengths
differ, total-wall differences cannot be attributed solely to cache; TTFT/prefill
remain separate. Record any quality failure and stop, rather than relaxing the
answer or output budget. Do not infer general language quality from this fixture.

Sample ONLY our server's live working set/private bytes at readiness, after
primer and after target, plus OS peak working set/commit, with controller memory
separate where available. Include model/libraries/KV/scratch in process costs;
mapped-file size and working set are different boundaries. Allocator breakdown,
GPU/VRAM, cold-machine behavior, other hardware, installed-app performance and
trained-model improvement remain unmeasured. No RAM-saving claim is expected.

## Review and execution boundary

Prepare fixtures/collector locally and run only reviewed nonmodel controls now.
Freeze protocol, fixture, collector/helper and identity hashes before submitting
the exact command/path bundle to parent. No model load, --help/--version binary
probe, serving request, settings change, upload/push/merge or Atlas update is
authorized by this preparation. An --execute flag is an opt-in safeguard, not
approval. Parent must review this exact protocol and code before live work.

Public-source semantics: pinned llama.cpp [cache bypass](https://github.com/ggml-org/llama.cpp/blob/3cb7ffb1a1f612d5e4a46244ae5a3c77ad934a70/tools/server/server-context.cpp#L2913-L2916),
[RAM cache](https://github.com/ggml-org/llama.cpp/blob/3cb7ffb1a1f612d5e4a46244ae5a3c77ad934a70/tools/server/server-context.cpp#L1201-L1210),
[final SSE statistics](https://github.com/ggml-org/llama.cpp/blob/3cb7ffb1a1f612d5e4a46244ae5a3c77ad934a70/tools/server/server-task.cpp#L439-L491),
[metric units](https://github.com/ggml-org/llama.cpp/blob/3cb7ffb1a1f612d5e4a46244ae5a3c77ad934a70/tools/server/server-common.cpp#L63-L75).

## Prepared command and nonmodel validation

From the task workspace, using the verified Python 3.12 interpreter, the proposed
command is one invocation (NOT run during preparation):

```text
python -B revia-rnl/research/rnl/prefix004.py --execute --identities .verification/prefix004-identities.json --reviewed-manifest .verification/prefix004-reviewed-inputs.json --stage .verification/prefix004-stage-01 --output .verification/prefix004-results-01
```

The private reviewed-input manifest contains exact protocol, driver, fixture,
helper and source-identity SHA-256 values. Its existence does not authorize
execution. The supplied paths are fresh task-owned destinations; no installed
settings, application server or existing port is reused. CPU-only staging is
still pending, as are the actual token-count gate, backend compatibility, cache
behavior and all performance measurements.

After independent source review, preparation passed 16 existing context-helper
tests, 15 data-only collector controls and three native ownership controls using
only isolated Python print/sleep children. The latter verified the larger job
limits, cleanup, independent deadline termination, redaction and log-cap stop.
They did not load llama.cpp, open sockets or exercise a model. Default
--validate-only also passed with live_metrics and performance_claim left null.

The first existing-helper test invocation had three OS permission errors in the
default sandbox TEMP. Its record is retained; the denied paths were not retried
or permission-changed. The unchanged tests passed in an explicitly task-owned
temporary directory. No proof of model speed follows from any of these controls.

Prepared publication paths are limited to PREFIX-004.md, PREFIX-004-identity.json,
prefix004-fixtures.json, prefix004.py, test_prefix004.py, README.md and
.gitattributes under research/rnl/. They remain local pending exact approval.
No Atlas payload is being published or merged in this preparation step.
