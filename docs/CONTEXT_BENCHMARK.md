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
