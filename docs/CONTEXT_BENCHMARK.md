# Local context benchmark

`Tools/Quality/contextBenchmark.py` runs three fixed synthetic probes at 16,384
and then 32,768 context tokens using an explicitly selected local llama.cpp
server and GGUF. This is direct backend evidence. It does not exercise Revia's
memory recall, context compaction, participant admission, restart recovery,
fallback routing, cancellation, personality, speech, vision or image workloads.
Concurrent workload responsiveness is **not measured**.

The tool starts a hidden, owned server process for each context, binds a fresh
loopback port, verifies its unique model alias and reported context/slot count,
and stops only that owned process in `finally`. It uses one generation slot,
eight CPU threads, all model layers requested on CUDA0, no GPU split, flash
attention, f16 K/V caches, batch 2048, microbatch 512, thinking disabled and
temperature zero. CUDA1 can be selected explicitly. Ambient `LLAMA_ARG_*`
settings are removed from the child environment. No Revia settings are changed.
Output must be a new directory; evidence is retained and never cleaned up.

## Reproduce

Use a working Python 3 installation and the installed executable/model paths:

```powershell
python Tools/Quality/contextBenchmark.py `
    --server C:/path/to/llama-server.exe `
    --model C:/path/to/Qwen3.5-4B-Q4_K_M.gguf `
    --output RuntimeData/Quality/context-benchmark-new-run
```

There are exactly three measured generations per context. Each prompt contains
an early fact, unrelated inventory/weather exchanges, a correction near 80% of
those exchanges, more distractors, then a final question. The probes cover:

1. Replacing four workshop decisions using a late correction.
2. Keeping Alice and Bob's conflicting preferences separate while changing only Alice's tea.
3. Retaining an early key location while replacing a completed task with the remaining task.

The backend's `/apply-template` and `/tokenize` endpoints determine exact prompt
cost, including special tokens and the generation marker. Binary search fills
approximately 75% of the configured context, keeping 192 output tokens reserved.
The completion uses the same messages and thinking setting, with JSON-object
output requested and prompt reuse disabled. Recorded usage is compared with the
preflight token count. The grader requires exact keys, values, value types, source
IDs and a `stop` finish reason; source strings and wrappers are not normalized
after observing results. A failing answer is retained unchanged.

The llama.cpp [server documentation](https://github.com/ggml-org/llama.cpp/blob/master/tools/server/README.md)
describes these endpoints and server options. The installed executable's help,
version, returned properties and actual responses determine this run's evidence.

## Observed run: 2026-10-06

Evidence directory on the development host:
`C:/Users/davis/OneDrive/Documents/GitHub/R.E.V.I.A/build/capability-evidence-20261006/context-benchmark-20261006`.
This is the preserved copy of the original worktree run; original launch paths
remain in its unchanged evidence.
This ignored runtime directory contains `run.json`, `summary.json`, per-context
launch/properties/results, server logs, and every exact request, rendered prompt
and response. It is local evidence and is not part of the source checkout.

- Model: Qwen3.5-4B-Q4_K_M.gguf, 2,740,937,888 bytes.
- SHA-256: `00fe7986ff5f6b463e62455821146049db6f9313603938a70800d1fb69ef11a4`.
- Server: 0.1.0-dev build 10453, commit `3cb7ffb1a`, Clang 20.1.8 Windows x86_64.
- CUDA0: RTX 5070, reported 12,227 MiB. CUDA1: RTX 2070 SUPER, reported 8,192 MiB.
- Model readiness: 3.96 seconds at 16K; 3.42 seconds at 32K. These separate sequential starts share host filesystem cache and are not cold-disk comparisons.

| Probe | 16K prompt tokens | 16K reply seconds | 32K prompt tokens | 32K reply seconds | Strict result at both sizes |
| --- | ---: | ---: | ---: | ---: | --- |
| Late decision correction | 12,232 | 3.600 | 24,511 | 6.241 | Fail: `source` was integer `2`, expected string `D2` |
| Participant attribution | 12,200 | 3.294 | 24,479 | 6.009 | Fail: source was `Source P2`, expected `P2` |
| Early fact and unfinished task | 12,239 | 3.375 | 24,518 | 6.214 | Fail: extra `KESTREL` wrapper and source `Source T2` |

Strict acceptance was **0/3 at each size**. Manual inspection of the unchanged
responses found all twelve requested non-source fact values correct at each
size, including locker 618 from the beginning of the conversation. This narrower
fact-recall observation does not turn the schema/source failures into passes.
All six generated replies stopped normally; all six usage counts exactly matched
the preflight token count and reported zero cached prompt tokens. The backend
reported no truncation. Wall latency includes HTTP, prompt processing and output
generation; it excludes prompt fitting and server startup. Time to first token
was not measured.

| Context | RTX 5070 baseline | RTX 5070 ready | RTX 5070 sampled peak | Peak above baseline | RTX 2070 SUPER sampled peak |
| --- | ---: | ---: | ---: | ---: | ---: |
| 16,384 | 2,202 MiB | 5,619 MiB | 5,632 MiB | 3,430 MiB | 91 MiB |
| 32,768 | 2,202 MiB | 6,148 MiB | 6,221 MiB | 4,019 MiB | 91 MiB |

GPU readings are per-device, system-wide `nvidia-smi` samples at approximately
two-second intervals. They include the desktop and any other processes and can
miss short peaks. They are not exact model allocations, nor are GPU capacities
added together. This server build did not emit individual buffer allocation sizes
at its default log verbosity. CUDA1 showed 91 MiB despite CUDA0 being the sole
selected model device; the evidence does not attribute those bytes. Both owned
server processes were confirmed stopped, and the post-run process inventory
contained no llama-server or Revia process.

The 32K run used approximately 589 MiB more sampled peak GPU memory and took
about 1.8 times as long for these longer prompts. Three synthetic questions with
repetitive distractors do not establish a general accuracy rate or justify a
production default. Context defaults remain unchanged. Follow-up acceptance
needs held-out natural conversations, pronoun-only and multilingual follow-ups,
actual Revia compaction/recall, restart and fallback, subjective personality
review, and concurrent speech/vision/art measurements.

## Default changed on 2026-10-18

`Config/settings.json` now gives Main `llm.contextSize: 32768` with
`llm.kvCacheType: "q8_0"` (passed as `--cache-type-k q8_0 --cache-type-v q8_0`,
which also switches `--flash-attn` from `auto` to `on`, because llama.cpp needs flash
attention for a quantized V cache). Fast and Expert keep their 8,192-token contexts and
the default f16 cache. The retained conversation history scales with Main's context:
about 60 messages and 60,000 characters at 32K, older messages kept up to about 12,000
characters, and the old caps at 8K and below. The resource planner adds about 40 MiB per
thousand tokens above 8K to the chat VRAM estimate (half that for q8_0), from the 16K and
32K readings above; llama.cpp's `--fit` still makes the final placement.

The run above used an f16 cache. The q8_0 cache at 32K has **not** been benchmarked with
this tool yet, and the change has only been unit-tested (launch arguments, history caps,
planner estimate, settings validation), not run live. To measure it, rerun the tool with
`--cache-type-k q8_0 --cache-type-v q8_0` in `server_arguments`.

## Prompt batch settings (added 2026-10-31, not yet measured)

`llm.batchSize` and `llm.microBatchSize` set Main's logical prompt batch (`--batch-size`,
tokens submitted per decode call) and physical micro-batch (`--ubatch-size`, tokens the
backend computes at once). Both default to `0`, which passes neither flag, so Main's
launch is unchanged until a value has been measured. Each must be `0` or an integer from
32 through 65,536, and a micro-batch needs an explicit batch at least as large; anything
else fails settings validation. Fast and Expert keep llama.cpp's defaults, as they do for
the KV cache type, and the embedding server keeps its own context-sized batch.

llama.cpp fits the batch to the context and the micro-batch to the batch, and `/props`
does not report either. After Revia launches Main, it reads that launch's own section of
`llama-server-<port>.stderr.log` and logs the values the context was actually built with,
for example `Prompt batch: 2048 logical, 512 physical tokens.`, or says they were not
found. `/status` shows the configured values. Prompt batching only changes how known input
is processed. It is separate from `llm.parallelRequests` (independent requests) and from
speculative decoding, which Revia does not configure.

Evidence so far: settings, launch arguments and log parsing are covered by
`Tests/promptBatchSettingsTests.cpp`. It ran on Linux against the real production owners,
and the configuration, flag and first-pair checks each failed when their code was removed.
That is compile and fixture evidence only. No batch value has been benchmarked, launched
or recommended. To qualify one on the host, first sweep prompt processing on Main's own
device, with Revia stopped so nothing else holds that GPU:

```powershell
.\ThirdParty\llama.cpp\llama-bench.exe -m Models\Qwen3.5-9B-Q4_K_M.gguf -dev CUDA0 -ngl 99 -fa on `
  -ctk q8_0 -ctv q8_0 -p 512,4096,16384 -n 0 -b 2048,4096 -ub 256,512,1024,2048 -r 3 -o json
```

A candidate then needs a Revia launch with the setting, the startup line confirming the
values, measured free VRAM with image or video work loaded (a larger micro-batch enlarges
the compute buffer that `--fit` must place), and first-visible-token and total latency for
long and ordinary turns compared with the default. Change the default only if those
measurements favor it.

## Quality observation proxy and checks

`Tools/Quality/observedLocalModel.h` now forwards the two accounting POST endpoints
using the same cancellable upstream client as generation. Status, response body
and content type survive forwarding. Accounting requests are deliberately omitted
from the completion evidence arrays so existing reviewer request indices remain
stable. The retained completion request still contains the actual final messages.
Existing `answerQualityLive` must be rebuilt to use this forwarding; this direct
backend run is not evidence from a rebuilt Revia live-quality executable.

Run Python contract checks with:

```powershell
python Tests/contextBenchmark.test.py
```

`Tests/observedLocalModelTests.cpp` is a standalone loopback fixture for exact
template forwarding, tokenization errors, completion index preservation and
unavailable-upstream handling. It requires C++20 and the repository's httplib and
nlohmann-json headers, with Winsock/Crypt32 on Windows. It can be compiled directly
without changing the shared CMake build. On this host the fixture was first run
against the old proxy and failed at missing template forwarding, then passed
after the endpoint change.
