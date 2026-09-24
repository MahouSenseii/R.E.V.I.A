# Revia voice performance report

This report records what was actually implemented and measured on the development desktop (RTX 5070, RTX 2070 Super, Ryzen 9 3900-class CPU). Goals are not reported as results. Anything not exercised is labeled **NOT VERIFIED**.

## Outcome

Revia now starts voice work from the first complete natural sentence, prepares the selected clone prompt during background startup, warms independent GPU workers concurrently, schedules the first sentence on the measured latency worker, avoids predicted slow-worker ordering stalls, and plays normal conversational audio from a bounded in-memory RIFF/WAV buffer. VoiceDesign runs in a separate on-demand process and cannot evict the conversational clone models.

The installed `qwen-tts` 0.1.1 API does **not** expose true incremental generated PCM. Its own source says `non_streaming_mode=False` only simulates streaming text input and does not enable streaming generation. Revia therefore implements **text-pipelined TTS with complete sentence audio**, not “true streaming audio.”

Qwen inference is still the dominant bottleneck. The result is substantially better startup/pipelining, but it is not yet real-time speech.

On the current single-GPU RTX 3070 Laptop system, Qwen3-TTS is already local PyTorch
inference. Automatic placement correctly keeps voice on CPU while the 8K-context 4B chat
model owns the GPU. Pinning both to CUDA was live-tested: it left 471 MiB free, increased
chat startup from about 7.5s to 36.5s, produced a 3.99s first-token wait (versus a
previous 1.06s auto-placement turn with a different prompt), and generated the short
spoken phrase in 24.13s. Shared placement is therefore not the default optimization for
an 8 GB card.

The conversational Qwen models intentionally remain GGUF models served by llama.cpp,
with GPU layer fitting. Moving the 4B chat model to PyTorch would consume more of this
card's limited VRAM and remove the measured fit/offload control; it is not treated as a
speed optimization on the 8 GB laptop.

## Architecture change

Before:

```text
LLM phrase -> Qwen full waveform -> temporary WAV -> HTTP JSON -> PlaySound(file)
```

Now:

```text
LLM tokens
   -> first complete natural sentence (28-character scheduling target)
   -> prompt-warm resident Qwen clone worker
   -> complete in-memory RIFF/WAV response
   -> bounded ordered C++ buffer
   -> PlaySound(SND_MEMORY)

following complete sentences (legacy 64-character target; never a split boundary)
   -> predicted latency/prefetch worker selection
   -> same ordered playback gate
```

Temporary playback WAVs are no longer required for normal conversation. WAV files remain for voice references, saved previews, vocalizations, debugging, and reproducible benchmarks.

## Reproducible baseline

Command:

```powershell
.\Tools\Benchmark-ReviaTTS.ps1 -Label baseline -Devices @('cuda:0','cuda:1')
```

Artifact: `RuntimeData/Benchmarks/TTS_QUENTINSPC-baseline-20260825-145006.json`

| Device | dtype | characters | generation total | audio | RTF |
|---|---|---:|---:|---:|---:|
| RTX 5070 | BF16 | 17 | 8.72s | 1.60s | 5.45 |
| RTX 5070 | BF16 | 32 | 11.85s | 3.12s | 3.80 |
| RTX 5070 | BF16 | 64 | 15.93s | 4.40s | 3.62 |
| RTX 5070 | BF16 | 99 | 19.34s | 5.36s | 3.61 |
| RTX 5070 | BF16 | 264 | 54.91s | 15.12s | 3.63 |
| RTX 2070 Super | FP32 | 17 | 9.56s | 2.08s | 4.60 |
| RTX 2070 Super | FP32 | 32 | 10.05s | 2.88s | 3.49 |
| RTX 2070 Super | FP32 | 64 | 11.54s | 3.52s | 3.28 |
| RTX 2070 Super | FP32 | 99 | 22.51s | 7.12s | 3.16 |
| RTX 2070 Super | FP32 | 264 | 50.92s | 16.32s | 3.12 |

Model-only preparation was 17.00s on the 5070 and 15.70s on the 2070 in this isolated run. These baseline artifacts predate GPU-resident-memory sampling; baseline VRAM is therefore **NOT RECORDED**, not guessed.

## Experiments and selected settings

### Clone prompt preprocessing

`POST /prepare-voice` now loads the 0.6B clone model, validates the reference WAV, builds the exact voice-clone prompt, and caches it by path, size, modification time, and transcript. Live dual-worker startups prepared both active prompts concurrently in 26–31s, down from observed sequential/model-only warmups of roughly 41–50s.

Prompt preprocessing removes first-use work and makes readiness truthful, but it does not make acoustic generation fast: a prepared 17-character comparison still took 10.24s under automatic attention.

### Text input mode

The complete-text mode was benchmarked across both GPUs. It helped some 5070 medium phrases but regressed several short/long 5070 samples and most 2070 samples. The measured default remains `simulated-stream`; the naming is intentionally precise because neither mode streams generated audio.

Artifact: `RuntimeData/Benchmarks/TTS_QUENTINSPC-optimized-20260825-151232.json`

### Attention backend

Prepared-prompt quick comparisons on the 5070:

| Backend | 17 characters | 64 characters | RTF (17 / 64) |
|---|---:|---:|---:|
| auto | 10.24s | 17.21s | 4.41 / 4.06 |
| eager | 9.89s | 17.37s | 4.94 / 4.62 |
| SDPA | **7.59s** | **15.78s** | **4.31 / 3.94** |

On the FP32 2070, explicit SDPA was stable but mixed relative to its baseline. The selected `adaptive` policy therefore chooses SDPA for native-BF16 devices and the package default for FP32/Turing devices. It detects dtype/capability; it does not match GPU names or assume `cuda:0`/`cuda:1`.

`flash-attn` is not installed. No verified Windows wheel was available in this environment for the pinned PyTorch 2.7.1/CUDA 12.8 and GPU architecture, so forcing `flash_attention_2` is **NOT RECOMMENDED**. PyTorch SDPA is the stable measured optimization.

### `torch.compile` and CUDA graphs

The installed Qwen wrapper and generation path contain no supported compile/graph integration, use dynamic autoregressive generation and custom caches, and are already decorated with `torch.no_grad`. No safe project-level compile target was identified. `torch.compile` and explicit CUDA graphs remain disabled and are **NOT BENCHMARKED** rather than being claimed as improvements.

### Ampere TF32

CUDA workers on Ampere or newer now enable PyTorch's high FP32 matmul precision mode and
TF32 for residual FP32 matrix/convolution work. The 0.6B clone weights remain BF16; CPU
and pre-Ampere workers retain their existing path. Interleaved quick samples on the RTX
3070 were noisy under mixed desktop load, but the repeated TF32 pass improved the
17-character sample from 10.12s to 9.15s and the 64-character sample from 23.34s to
21.39s. An earlier pair improved from 7.64s/16.19s to 7.08s/13.70s. This is a bounded
kernel optimization, not a claim of real-time generation.

### Direct memory playback

The Python worker encodes the completed waveform into an in-memory RIFF/WAV response. C++ keeps those bytes alive in the existing bounded ordered queue and uses Windows `PlaySound` with `SND_MEMORY`. Response headers carry queue, model, prompt, generation, encode, audio-duration, dtype, attention, and cache timings.

This removes the normal temporary-file round trip, but measurements confirm that file/HTTP overhead is tiny beside multi-second inference. Direct memory playback is primarily a cleanliness, cancellation, and buffer-ownership improvement; it is not presented as the main latency win.

## Live results

| Check | Before | After |
|---|---:|---:|
| Background voice warmup | roughly 41–50s observed | 26–31s |
| Real-session queue to first audible phrase | 33.1s observed | 17.1s observed |
| Ordered phrase two | 43.5s queue-to-play in prior run | ready at 19.56s and played at 19.92s in the new run |
| Reflex C++ response (without voice generation) | not separately measured | 18ms |
| Repeated cached “Okay.” to audible playback | generative path only | **65ms** (46ms ready) |

The before/after live spoken texts were not identical, so the reproducible phrase matrix—not the live anecdote—is the correct model-performance comparison.

The second new phrase was ready while phrase one was playing and began immediately after it, preserving order without an audible scheduling gap in the event timings. A Stop command cleared queued playback and stale results; the following uncached “Okay.” still waited for an in-flight non-cancellable Qwen request. A strict nine-phrase, in-memory-only Reflex cache now makes repeated deterministic acknowledgements bypass generation. Arbitrary/private conversation is never added to that cache.

## Residency and scheduling

- Conversational 0.6B clone workers stay resident after startup.
- VoiceDesign 1.7B has an isolated on-demand process and shuts down after creation; it cannot evict clone workers.
- Independent workers prepare concurrently.
- The first complete sentence is latency-critical and waits for the primary measured worker rather than being handed to an arbitrary free worker.
- Following work compares predicted fixed overhead plus character cost and waits for a faster busy worker when a slower free worker would finish later.
- Playback remains sequence-ordered.
- RTX 2070/Turing remains FP32. BF16 and the previously unstable FP16 path are not reintroduced.
- Audio bytes, prefetched phrases, request queues, and output buffers remain bounded by configured limits.

## Recommended settings

```json
{
  "qwenMaxWorkers": 2,
  "qwenPrefetchFragments": 3,
  "qwenFirstPhraseCharacters": 28,
  "qwenPhraseCharacters": 64,
  "qwenMaxBufferedAudioMiB": 128,
  "qwenMinimumFreeVramMiB": 4600,
  "qwenDirectPcm": true,
  "qwenPrecomputeVoicePrompt": true,
  "qwenAttentionBackend": "adaptive",
  "qwenInputMode": "simulated-stream"
}
```

The resource planner still chooses `qwenDevice`, `qwenDevices`, and the effective worker count from detected hardware and free capacity.

## Verification matrix

| Item | Result |
|---|---|
| Clean configured build | PASS |
| Foundation/browser/Qwen HTTP/desktop tests | PASS (4/4) |
| Typed chat and Reflex | PASS, live |
| Fast 0.8B startup/answer | PASS, live |
| Main 4B startup/answer | PASS, live |
| Expert 8B startup/difficult turn | PASS, live after bounded-context fix |
| Expert fallback | Implemented and unit-covered through routing contracts; forced live outage **NOT VERIFIED** |
| Active clone prompt preparation | PASS, live |
| Direct in-memory playback | PASS, live |
| Two-fragment ordering | PASS, live and unit-tested |
| Stop/stale-result suppression | PASS in live sample; extended stress **NOT VERIFIED** |
| Barge-in microphone trigger | Existing implementation/tests pass; physical spoken-over stress in this pass **NOT VERIFIED** |
| VoiceDesign isolation | Implemented; full new-voice live generation in this pass **NOT VERIFIED** |
| Vocalization endpoint bug | FIXED; loopback HTTP regression PASS |
| Windows SAPI fallback | Existing live-test entry point retained; not invoked in this pass |
| Physical one-GPU laptop | PASS: isolated CPU/CUDA plus live shared-GPU pressure tested |
| CPU Qwen synthesis | PASS: 17 chars 34.22s; 64 chars 72.25s |
| Dual-GPU Qwen generation | PASS, live and benchmarked independently |
| Long-duration RAM/VRAM/handle leak | **NOT VERIFIED** |
| Worker cleanup | PASS for all completed live/benchmark runs |
| Fresh-clone setup on another PC | **NOT VERIFIED** |

## Files and commands

Core implementation lives in `Tools/qwen_tts_service.py`, `Private/Speech/`, `Public/Speech/`, `Private/Agents/replyFragmenter.cpp`, and the speech configuration in `Config/settings.json`. Setup/verification changes are in `Tools/InstallQwenTTS.ps1`, `Tools/HealthCheck.ps1`, and `Tools/Benchmark-ReviaTTS.ps1`.

Run a full local matrix with:

```powershell
.\Tools\Benchmark-ReviaTTS.ps1 -Label comparison -Devices @('cuda:0','cuda:1')
```

The remaining bottleneck is Qwen acoustic generation itself. Future work should only claim a true streaming improvement when an installed supported API actually yields incremental audio chunks.

---

# Pass 2: batched throughput and the first-phrase floor

Everything above is preserved as the earlier baseline. This section records a later pass
on the same development desktop (RTX 5070, RTX 2070 Super, Ryzen 9 3900X-class, 128 GB).
Goals are not reported as results, and anything not exercised is labeled **NOT VERIFIED**.

## The two problems are different

**Throughput** asks whether Revia can generate future speech faster than playback
consumes it. **Time to first audio** asks how quickly the listener hears the first newly
generated word. Batching addresses the first and cannot address the second, because the
first phrase of a reply has nobody to share a call with. They are reported separately
throughout, and a batch real-time factor is never quoted as a latency result.

## Baseline: per-phrase generation, RTX 5070, warm resident model

| sample | chars | generation | audio | RTF |
|---|---|---|---|---|
| ~16c | 15 | 5.57 s | 1.60 s | 3.48 |
| ~32c | 34 | 6.66 s | 2.08 s | 3.20 |
| ~64c | 62 | 11.01 s | 3.52 s | 3.13 |
| ~100c | 100 | 16.98 s | 5.44 s | 3.12 |
| ~250c | 260 | 43.21 s | 13.76 s | 3.14 |

The vocoder accounts for about 0.08 s of any of these; roughly 99% is autoregressive
generation. The RTX 2070 Super in float32 measured RTF 2.83–3.22 over the same samples —
slightly *faster* than the 5070 on longer text, which is the first evidence that raw
arithmetic was never the constraint.

## Why a 0.6B model spent 16 ms a step

`num_code_groups` is 16. Every 12 Hz audio frame runs one talker step and then a nested
Hugging Face `.generate()` call that samples fifteen sub-tokens one at a time. Measured
on a 62-character phrase producing 3.36 s of audio: 672 sequential forward passes at
16.4 ms each, with 70.7% of the time inside 42 nested `.generate()` invocations.

That last figure was originally read as Hugging Face overhead. It is not. Replacing the
nested call with a direct forward loop (below) returned only 1.07× on the inner loop, and
instrumenting it showed why: a single predictor forward costs about 11 ms on its own, and
the generic generation machinery around it is roughly 6.5% of the total. **The time is
inside those calls, but it is the forward passes, not the wrapper.**

What the forward passes are actually spending it on:

| measurement (RTX 5070, bfloat16) | time |
|---|---|
| predictor forward, 5 layers, 141 M params | 11.13 ms |
| talker forward, 28 layers, 906 M params | 64.21 ms |
| one `lm_head` Linear(1024, 2048) | 0.097 ms |
| bare 1×1024 @ 1024×2048 matmul | 0.0397 ms |
| empty kernel launch | 0.0257 ms |

Both models cost about **2.2 ms per transformer layer regardless of size**, and a
predictor forward costs the same at batch 32 as at batch 1 (1.04×). The work is
per-launch and per-dispatch overhead — amplified by Windows WDDM, where an empty launch
already costs 26 µs — not arithmetic.

That single fact explains both results in this pass. Batching amortises the overhead
across sequences. CUDA graphs remove it.

## Batched replies: throughput

The installed API already accepts a list of phrases. Phrase counts and their measured
real-time factors on the 5070:

| phrases in one call | RTF |
|---|---|
| 1 | 3.23 |
| 2 | 1.64 |
| 3 | 1.18 |
| 4 | 0.97 |
| 5 | 0.82 |
| 6 | 0.58 |

**The crossing below 1.0 happens between three and four phrases.** A reply that makes
only two or three phrases still improves by 2–3× but does not reach the point where
synthesis outruns playback. This is a real limit of the change and is not rounded away.

Production client to production worker, batch of five:

| | generation | audio | RTF | peak VRAM | resident | prompt cached |
|---|---|---|---|---|---|---|
| cold | 10.41 s | 12.56 s | 0.83 | 3442 MiB | no | no |
| warm | 10.60 s | 12.96 s | 0.82 | 3477 MiB | yes | yes |

### Two bugs found while building it

**The PyTorch caching allocator made the VRAM gate refuse every batch after the first.**
The gate read `torch.cuda.mem_get_info`, which reports driver-free memory and excludes
blocks PyTorch has taken and is no longer using. After one batch the arena stays
reserved, so a card with gigabytes of reusable cache looked full: *"Projected 624 MiB
plus 1150 MiB reserve exceeds 1609 MiB free."* Every later batch would have fallen back
to the per-phrase path silently — the failure mode that looks like a working feature
doing nothing. The gate now adds `memory_reserved - memory_allocated`.

**The batch size estimate was fitted on the wrong variable.** A per-character model
underestimated batches of several short phrases by more than 2×: five phrases totalling
195 characters cost 1412 MiB, where four longer phrases totalling 472 characters cost
1565 MiB. Each sequence carries a fixed cost. The estimate is now
`1.5 MiB/char + 240 MiB/phrase`, fitted across batches of 4, 5, 8 and 12 and doubled for
float32.

VRAM scaling that set the character ceiling:

| phrases | chars | generation | audio | RTF | peak alloc | card total used |
|---|---|---|---|---|---|---|
| 1 | 118 | 18.11 s | 5.60 s | 3.23 | 2457 MiB | 3898 MiB |
| 4 | 472 | 22.88 s | 24.88 s | 0.92 | 3677 MiB | 6614 MiB |
| 8 | 944 | 22.24 s | 49.20 s | 0.45 | 5143 MiB | 10583 MiB |
| 12 | 1416 | 22.36 s | 73.28 s | 0.31 | 6602 MiB | 12226 MiB |

Twelve long phrases consumed the entire 12 GiB card. The 5070 also holds chat and vision
weights, so `qwenMaxBatchCharacters` is 480 — about 1.6 GiB above the resident model,
still reaching RTF ≈ 0.9.

## Attacking the first phrase

The first phrase of a reply cannot be batched — it is the only latency a listener
experiences, and holding it back to collect company for it would trade the thing that
matters for a number that does not. So it needed a different lever.

### Removing nested `.generate()` from the hot loop — done, and it barely mattered

`Tools/qwen_lowlatency.py` replaces the nested Hugging Face `.generate()` in the
codebook predictor with a direct forward loop that carries its own cache and samples
inline. Hugging Face still loads and defines the model; only generic `.generate()` is
gone from the inner loop.

Sampling is reproduced exactly rather than approximately: divide by temperature, keep the
top `k`, softmax, one `multinomial` draw — the same operations in the same order Hugging
Face builds for this configuration (`top_p` is 1.0 upstream, at which value Hugging Face
omits the warper entirely, so it is omitted here). `output_hidden_states` is dropped
because the caller discards it.

**Verified token-for-token: 6/6 trials under a fixed seed produced byte-identical token
streams through both paths.** That is a stronger check than comparing audio, which would
only show two stochastic samplers agreeing roughly.

The payoff was small:

| inner loop, one audio frame | median | p90 | per sub-token step |
|---|---|---|---|
| Hugging Face `.generate()` | 183.15 ms | 198.85 ms | 12.21 ms |
| direct forward loop | 170.66 ms | 186.83 ms | 11.38 ms |

**1.07×.** The instrumented breakdown says why: 15 forwards at a mean of 10.9 ms, 164 ms
of the 171 ms total. The generic machinery is about 6.5% of the cost. The earlier reading
that 70.7% of time sat "inside nested `.generate()` calls" was true and misleading — the
time is inside them, but it belongs to the forward passes, not the wrapper.

### CUDA graphs — 13.1× measured, and not yet safe to ship

The predictor forward costs the same at batch 32 as at batch 1, and a comparable bare
matmul runs in 0.04 ms against a 11.13 ms forward. That is per-launch overhead, and a
graph collapses hundreds of launches into one submission.

Captured in isolation, on the RTX 5070:

| | |
|---|---|
| one decode step, eager | 11.166 ms |
| one decode step, graph replay | 0.810 ms (**13.79×**) |
| whole 15-step inner loop, eager | 171.91 ms |
| whole 15-step inner loop, graph replay | 13.089 ms (**13.13×**) |
| capture cost | 283 ms, once |
| extra VRAM | ~30 MiB |

The graph is real, not a recorded constant: replays respond to changed input, the random
stream still advances between replays, and sampled tokens stay in range.

**It does not survive integration.** Driven by the talker's own generation loop it fails
two different ways: capturing at model load produced a graph that replayed correctly
alone but hung the first time the full pipeline ran it, and capturing lazily on the first
real phrase instead produced `index_copy_(): index out of bounds` from the cache kernel.
The live flow's prefill shape was confirmed to be exactly the captured one — 19 calls of
`(1, 2, 1024)` with `max_new_tokens=15` — so this is not a shape mismatch. The likely
cause is index bookkeeping inside Hugging Face's `StaticCache` that a replay cannot
reproduce.

The code is kept, marked experimental, and **off by default** (`qwenCudaGraph: false`).
A voice that occasionally crashes its worker is worse than a voice that is slow. The
well-defined next step is to give the graph a cache it owns outright — preallocated
key/value tensors written at baked offsets — instead of borrowing one whose internal
indexing is invisible from the call site.

### `torch.compile`

Not viable in this environment, verified rather than assumed: `triton` is not installed,
`torch.utils._triton.has_triton()` returns `False`, and the Inductor backend requires it
on Windows. No dependency was added to claim support. `torch 2.7.1+cu128`,
`transformers 4.57.3`.

## Configuration

```jsonc
"qwenBatchReplyPhrases": true,    // phrases 2+ share one generation call
"qwenMaxBatchPhrases": 6,
"qwenMaxBatchCharacters": 480,    // ~1.6 GiB above the resident model
"qwenLowLatencyPhrase": false,    // direct-forward inner loop (token-exact, ~1.07x)
"qwenCudaGraph": false            // EXPERIMENTAL: 13.1x isolated, unstable integrated
```

Validation stays active whether or not the flags are on, so enabling one later cannot
activate a value that was never checked: 2–32 phrases, 64–4096 characters.

## Logging

```text
[Voice] request #N | device=... dtype=... attention=... resident=... prompt_cached=...
                     queue_depth=... vram=.../...MiB gpu_util=...% audio=...ms rtf=...
                     backend=standard|low_latency cuda_graph=yes|no
[Voice] batch #N   | phrases=... characters=... generation_ms=... audio_ms=...
                     batch_rtf=... peak_vram_mib=... device=... queue_depth_before=...
                     queue_depth_after=... backend=... cuda_graph=... fallback=no
```

Every fallback is logged as a **warning**, because a batch that silently never runs
looks exactly like a working feature.

## Verification status

| item | status |
|---|---|
| Clean C++ build | PASS |
| `ReviaTests.exe` | PASS |
| Python TTS tests (11, incl. 8 new) | PASS |
| Batch framing unit tests (11 cases) | PASS |
| Production client → production worker | PASS, cold and warm |
| Cold batch of 5 | PASS, RTF 0.83 |
| Warm batch of 5 | PASS, RTF 0.82 |
| Repeated warm batches | PASS (caught the cached-allocator bug) |
| VRAM gate regression | PASS, unit-tested both directions |
| Malformed/wrong-count batch fallback | PASS, unit-tested |
| Single-phrase benchmark, both GPUs | PASS |
| Token-exact equivalence, direct loop | PASS, 6/6 |
| CUDA graph in isolation | PASS, 13.1× |
| CUDA graph integrated | **FAIL — disabled by default** |
| `torch.compile` | Not viable (no Triton on Windows) |
| **Live conversation with batching** | **NOT VERIFIED** |
| **Live mid-batch interruption** | **NOT VERIFIED** |
| **Perceptual voice quality of batched output** | **NOT VERIFIED — paired WAVs produced for listening** |
| Long-reply queue-depth-over-time trace | **NOT VERIFIED** |
| True incremental audio | Still unavailable in `qwen-tts` 0.1.1 |

## Known limitations

- Replies of two or three phrases stay above RTF 1.0 (1.18–1.64). Only four or more
  cross below.
- First-phrase latency is essentially unchanged. The direct-forward loop returns 1.07×,
  and the change that would matter — CUDA graphs — is not yet safe to enable.
- The batch path is batch-1-incompatible with the graph by construction, which is fine:
  the two paths solve different problems.
- Nothing here has been heard by a human. Structural checks (valid RIFF/WAVE, distinct
  clips, duration within range, correct ordering) all pass, but perceptual equivalence
  is unestablished.

---

# Pass 3: the graph-owned cache, and Stage 1 working

The previous pass measured 13.1× for a graphed predictor inner loop in isolation and
could not make it survive the real talker loop. Two distinct bugs were responsible. Both
are now fixed, and the graphed predictor runs in production generation.

## Bug 1 — Hugging Face `StaticCache` cannot be captured here

`StaticCache` writes with `index_copy_` driven by a `cache_position` tensor and keeps
bookkeeping a replay does not reproduce, which surfaced as
`index_copy_(): index out of bounds` once the talker drove the graph.

It is replaced by `GraphOwnedPredictorCache`, built for the one shape the predictor
actually has rather than generalised:

* **Fixed storage.** Every layer's keys and values are allocated once before capture and
  never reallocated, so addresses recorded at capture stay valid for every replay.
* **Baked offsets.** `write_offset` and `write_length` are plain Python integers set
  before each forward, so slice bounds are constants folded into the captured kernels.
  No tensor indexing, no `index_copy_`.
* **A narrow view instead of a mask.** `update` returns exactly the valid prefix. With
  `attention_mask={"full_attention": None}` the model's mask construction is skipped
  entirely and SDPA derives causality from the query length — true for the two-token
  prefill, false for a single decode step attending over its whole past. No mask is
  built, and nothing needs zeroing between phrases because a position is always written
  before it can be read.

Verified without any graph involved: the owned cache produces **token-identical** output
to `DynamicCache` on the same input.

## Bug 2 — the captured inputs were freed

This was the expensive one. The cache-position tensors were a local list inside
`capture()`. A CUDA graph records the *addresses* of its inputs; when `capture()`
returned, the list was collected, the allocator handed that memory to something else, and
every replay read whatever now sat there as its cache positions. That corrupts the rotary
embedding from the first decode step onward.

The symptom was misleading in a specific way: the prefill token still matched, so output
looked *almost* right, which pointed at the cache rather than at object lifetime. And
because the corrupted codes never produced an end-of-sequence, the talker ran to
`max_new_tokens=4096` — a phrase that should take 19 frames was still going at 447, which
looked exactly like a hang.

The positions now live on the instance. `Tests/qwenTtsService.test.py` guards the
property that made the bug possible: every tensor the captured loop consumes must be
reachable from the object owning the graph.

## Correctness gates, all passing

The direct-forward path is the oracle, since it is itself token-exact against stock
Hugging Face generation.

| gate | result |
|---|---|
| graph vs direct-forward, greedy, fixed seed | **token-exact** |
| replay uses current input, not captured values | different input → different codes |
| greedy replay reproducible | 6/6 identical |
| sampling: RNG advances between replays | yes |
| sampled tokens within vocabulary | yes |
| owned cache vs `DynamicCache`, eager, no graph | token-exact |
| generated audio finite, non-silent | yes, peak 0.328 |

Greedy is used for the exact comparison deliberately: a graph carries its own random
stream, so sampled output cannot be compared token-for-token against eager. Determinism
is asserted where it must hold, and RNG advance is asserted where it must not.

## Stage 1 result — graphed predictor in real generation

RTX 5070, warm resident model, cached clone prompt, `"Yeah, I see it."`:

| | generation | audio | RTF |
|---|---|---|---|
| stock Hugging Face | 5.65 s | 1.68 s | 3.36 |
| **graphed predictor** | **1.70 s** | 1.52 s | **1.12** |

**3.33× on a real phrase.** Peak VRAM 3802 MiB — about 300 MiB above the resident model.
Capture costs ~300 ms once per model load.

Capture happens at install, a quiescent point with no generation in flight. Capturing
lazily from inside the talker's own loop was tried and is not safe.

## Where the time goes now

For an 18-frame phrase at the measured per-frame costs:

| | before | after |
|---|---|---|
| talker forward, 28 layers | ~64 ms/frame | ~64 ms/frame |
| predictor inner loop | ~172 ms/frame | ~13 ms/frame |
| **per frame** | **~236 ms** | **~77 ms** |

The talker is now roughly 68% of generation time, which is what Stage 2 has to attack.
It is the same shape of problem — 28 layers at ~2.3 ms each, flat across batch size — so
the same lever should apply, but it sits inside Hugging Face's own generation loop rather
than in a helper this code owns, which makes it a larger change.

---

# Pass 4: Stage 2 — graphing the talker

Stage 1 left the talker at roughly 68% of generation time: ~64 ms a frame against the
graphed predictor's ~13 ms. Stage 2 replays the talker's decode step from a graph too.

## What was graphed, and what deliberately was not

Only `talker.model` at query length 1 — the 28-layer inner stack. Prefill stays eager
because its length varies and it runs once per phrase. Sampling, end-of-sequence
detection, the token budget and the codebook predictor call all remain Hugging Face's.

That boundary is deliberate. The Stage 1 lifetime bug corrupted hidden state in a way
that never produced an end-of-sequence, so a 19-frame phrase was still running at 447
frames and looked exactly like a hang. A graph that took responsibility for stopping
would make that failure mode easier to reach, not harder.

## A different cache from the predictor's

The predictor always runs fifteen sub-steps, so its cache could bake slice offsets as
Python integers. The talker cannot: its length is the voice-clone prompt plus however
many frames end-of-sequence decides on. Baking an offset per step would mean one graph
per frame.

`FixedTalkerCache` is therefore fixed-width with the write position held in a **tensor**,
read at replay rather than recorded at capture, so one graph serves every decode step.
The attention mask is built device-side from that same position (`arange <= position`)
and handed in as a 4D mask, which makes the model's own mask construction early-exit as a
passthrough.

Sizing was measured, not guessed: prefill is 86 positions and a 250-character phrase
reaches position 252, so the width is 512 — roughly twice the longest phrase — with an
eager fallback rather than wrapping if a phrase ever exceeds it.

## Results — RTX 5070, warm resident model, sequential runs, 3 repeats

| sample | chars | stock | direct-forward | Stage 1 | **Stage 1+2** | speedup |
|---|---|---|---|---|---|---|
| ~16c | 15 | 7.13 s / 3.71 | 6.41 s / 2.97 | 2.38 s / 1.06 | **0.65 s / 0.34** | 11.0x |
| ~32c | 34 | 8.54 s / 4.11 | 6.72 s / 3.00 | 2.55 s / 1.06 | **0.69 s / 0.33** | 12.4x |
| ~64c | 62 | 13.42 s / 3.36 | 11.64 s / 2.97 | 4.01 s / 1.04 | **1.07 s / 0.31** | 12.5x |
| ~100c | 100 | 20.82 s / 3.88 | 15.94 s / 2.97 | 5.74 s / 1.03 | **1.65 s / 0.30** | 12.6x |
| ~250c | 260 | 46.70 s / 3.24 | 42.01 s / 2.97 | 13.80 s / 1.01 | **4.07 s / 0.29** | 11.5x |

Cells are generation time / real-time factor. Zero failures in twenty cells; p90 equals
median at every point. Peak VRAM 4472–5188 MiB. Capture costs ~210–310 ms once per model
load, per graph.

Per-frame component costs behind those totals:

| | before Stage 1 | after Stage 1 | after Stage 2 |
|---|---|---|---|
| talker decode (28 layers) | ~64 ms | ~64 ms | **~5.5 ms** |
| predictor inner loop | ~172 ms | ~13 ms | ~13 ms |
| per frame | ~236 ms | ~77 ms | **~18.5 ms** |

The real-time factor is now *better* on longer phrases, which is the reverse of the
original behaviour and follows from fixed per-phrase overhead being amortised.

## The bug worth recording

The first Stage 2 matrix reported the graphed talker as no faster than Stage 1. That was
not a result; it was a silent no-op:

```python
if self.inner.forward is not self.original_forward:
    return
```

Attribute access on a bound method builds a new object every time, so the identity test
was always true and `install()` returned before routing anything. Nothing raised.
Generation simply stayed on the eager path and an entire benchmark column was wrong.

It was caught because the replay counters read zero. Installation state is now an
explicit flag, `Tests/qwenTtsService.test.py` asserts that the identity comparison is
gone, and the counters exist precisely so a no-op is visible rather than plausible.

Two smaller contract bugs preceded it: `capture()` ran before `original_forward` was set,
and the routed forward returned `hidden_states=None` where the outer loop reads
`hid[0][-1]` for every step.

## Correctness gates

| gate | result |
|---|---|
| no runaway generation (frame count vs stock) | 23 -> 17, ratio 0.74 |
| audio duration within 40% of stock | yes |
| audio finite, non-silent, no clipping | peak 0.367 |
| repeated generation, 5 phrases | no leakage |
| output length spread vs stock control | **1.88x vs stock's 2.83x** |
| median duration vs stock | 1.84 s vs 1.80 s |
| VRAM over 10 further phrases | 0 MiB growth |
| eager fallbacks | 0 |
| replay follows current input and position | yes |

The length spread deserves a note: frame counts vary between runs of the same text, which
initially read as instability. Running stock as a control showed the spread is the
temperature-0.9 sampler, and that Stage 2 is in fact *tighter* than stock. The gate was
wrong, not the implementation.

## Configuration

```jsonc
"qwenLowLatencyPhrase": false,   // direct-forward inner loop
"qwenCudaGraph": false,          // Stage 1: graphed predictor
"qwenTalkerGraph": false         // Stage 2: graphed talker; requires qwenCudaGraph
```

Three separate switches, all defaulting off. Stage 2 requires Stage 1 and does nothing on
its own; a talker graph that fails to capture leaves Stage 1 running rather than taking
it down. Logs carry `backend=`, `cuda_graph=` and `talker_graph=` per request and per
batch, so which path actually served a phrase is never inferred.

## Still not verified

Perceptual A/B listening, live conversational running, and live mid-batch interruption
remain **NOT VERIFIED**. Paired WAVs across all four backends and five phrase lengths are
written for listening, but no human has heard them.

---

# Pass 5: regression, stability, and what batching is now for

## Two bugs found in this pass

**The graph captured empty on a secondary GPU.** `torch.cuda.graph()` records on
whichever device is *current*, not on the device the tensors live on. Capturing a cuda:1
model while cuda:0 was current produced an empty graph — PyTorch warns rather than
raising, so it replayed nothing, left whatever was already in the static output buffer,
and the numbers looked plausible. Capture and replay are now scoped to the owning device,
and capture asserts that an output tensor actually exists. Found only because the RTX 2070
leg of the matrix printed the warning.

**Stage 2 broke batched calls.** The talker cache and graph are shaped for a single
sequence; a batched call has batch N and was still entering the routed forward, writing
keys of the wrong shape into storage sized for one. Enabling Stage 2 alongside batching
would have broken the working batch path. The router now falls back to eager for any
batch other than one.

Both are the same species as the Stage 1 lifetime bug: a graph does what it was recorded
doing, and every assumption baked in at capture has to be checked at replay.

## Stability gates — RTX 5070, all passing

| gate | result |
|---|---|
| capture produces real output tensors | predictor 315 ms, talker 243 ms |
| 20-phrase stress | gen median 0.69 s, RTF median 0.37, max 0.48 |
| eager fallbacks during stress | **0** across 455 predictor and 455 talker replays |
| live allocations over 20 phrases | **2743 MiB -> 2743 MiB, zero growth** |
| allocator arena | 2926 -> 3178 MiB (reserved and reused, not leaked) |
| cache reuse | talker 512x28 and predictor caches reused, never rebuilt |
| cache overflow (24-slot window, long phrase) | 182 eager steps, audio valid |
| both graphs disabled mid-session | eager path works, RTF 3.34 |
| recovery after fallback | RTF 0.36 |
| uninstall | stock path restored |

The VRAM figure is worth stating precisely, because the first measurement looked like a
leak: driver-reported usage grew about 300 MiB over twenty phrases, which is PyTorch's
caching arena expanding and then reusing. Live allocations are exactly flat.

Batch regression, production client to production worker: cold RTF 0.86, warm RTF 0.83,
all framing and ordering checks passing. Unchanged by Stage 1 and 2.

## Batching is no longer the right default

Batching existed because a single sequence ran at real-time factor 3.2 and the only way
to keep ahead of playback was to amortise per-launch overhead across sequences. CUDA
graphs remove that overhead directly, so the premise was re-tested rather than inherited.

A realistic six-phrase reply, RTX 5070, Stage 1 and 2 enabled:

| | A: phrase 1 graphed, 2+ batched | B: every phrase graphed |
|---|---|---|
| first audio ready | 1.03 s | **0.97 s** |
| all generation done | 10.93 s | **5.82 s** |
| overall real-time factor | 0.72 | **0.35** |
| gaps mid-reply | **1 — 6.86 s of silence after phrase 1** | **0** |
| peak VRAM | 6528 MiB | **4554 MiB** |

The gap is the point, and it is structural: **no phrase in a batch can play until every
phrase in that batch has finished**. Phrase one plays for 2.6 s and then the listener
waits 6.86 s for the batch behind it. Trading per-phrase availability for throughput was
the right trade at RTF 3.2; at RTF 0.35 it buys nothing and costs a silence in the middle
of a sentence, plus 2 GB.

**Recommended production shape:** every phrase generated independently on the graphed
path. Batching is kept, still correct and still tested, as the fallback for when graphs
are unavailable — where it remains a large win over eager per-phrase generation
(RTF 0.83 against 3.2).

## Recommended configuration

```jsonc
"qwenLowLatencyPhrase": true,    // direct-forward inner loop
"qwenCudaGraph": true,           // Stage 1: graphed predictor
"qwenTalkerGraph": true,         // Stage 2: graphed talker
"qwenBatchReplyPhrases": true    // retained as the fallback path, not the default route
```

All four still default to **false** in the shipped `Config/settings.json`. Nothing has
been run in a live conversation yet, and that is the gate for changing the defaults.

Provisional placement: the RTX 5070 is the latency-critical TTS worker. The RTX 2070
Super remains available for speech recognition, secondary phrase-ahead work and fallback;
its full graphed matrix was stopped rather than delaying completion, since its FP32 eager
legs dominate the runtime and the production question was already answered.

## What generation time is, and what it is not

Every figure in this document is **TTS generation time**: text in, waveform out, on a
warm resident model with a cached clone prompt. A 15-character phrase generates in
0.65 s.

Audible time to first audio is a different quantity and has **not** been measured:

```
user stops speaking -> LLM first token -> first complete natural phrase
                    -> TTS request -> generation -> transport -> playback start
```

Generation is now one term in that chain rather than the dominant one. Sub-second
*audible* response remains unproven until it is measured in the running application.

## Not verified

- **Perceptual A/B voice quality.** Structural checks pass — valid RIFF/WAVE, finite,
  non-silent, no clipping, distinct clips, duration distribution tighter than stock — but
  no human has listened. Paired WAVs across four backends and five phrase lengths are
  written for that purpose.
- **Live conversational time to first audible audio.**
- **Live mid-batch interruption.**

These need the application driven by a person and are not inferred here.

---

# Pass 6: the live baseline, and turning the graph path on

## The measurement Pass 5 was waiting for

Pass 5 ended with "live conversational time to first audible audio" listed as not
verified, and the four configuration flags left at `false` pending "a live conversation
run". That run happened on **2026-09-02, 17:37–18:04**, 27.6 minutes, 7 turns
(`Logs/revia.log`). It ran entirely on the stock path — all 116 voice requests in the
log file, going back to August, report `backend=standard cuda_graph=no talker_graph=no`.

So the baseline is a measurement of the old path, which is exactly what was missing:

| turn | first audible audio | phrases | reply finished speaking |
|---|---|---|---|
| 1 | 19.6 s | 8 | 130.8 s |
| 2 | 22.6 s | 11 | 164.1 s |
| 3 | 23.2 s | 12 | 175.4 s |
| 4 | 27.6 s | 8 | 153.1 s |
| 5 | 18.0 s | 6 | 95.3 s |
| 6 | 34.8 s | 15 | 285.4 s |
| 7 | 17.8 s | 6 | 161.1 s |

The language model was not the constraint: mean `turn_total` was 3.0 s excluding the one
turn that did an internet lookup. TTS spent **1516 s of generation to produce 342 s of
audio** in a 1655 s session — the queue could not drain, and every reply fell further
behind inside itself. Live real-time factor was **4.13** on the RTX 2070 Super (49
requests) and **5.78** on the RTX 5070 (17 requests), against the 0.29–0.34 measured for
the graphed path in Pass 4.

## What changed

`qwenLowLatencyPhrase`, `qwenCudaGraph` and `qwenTalkerGraph` now default to `true` in
`Public/Library/structLibrary.h` and are `true` in `Config/settings.json`. Both matter:
the shipped settings file overrides the defaults, so changing one without the other
leaves the runtime resolving to whatever the file says.

**Verification, because a flag is not a measurement.** Three separate facts are now
reported rather than one:

- `low_latency_requested` — what the configuration asked for.
- `low_latency_installed` / `low_latency_state` — whether the module loaded on this
  worker. Known when the clone model loads; `not-loaded` before that, which is not the
  same as `unavailable` and no longer reads as it.
- `cuda_graph` / `talker_graph` — whether a graph object actually exists to replay.

At startup, after the voice warms, the session logs the resolved state:

```
[Voice] low_latency_phrase=yes predictor_graph=yes talker_graph=yes
```

and emits a **warning** in the same shape if the path was requested and did not install,
naming the device and quoting the worker's reason. The first phrase of each session is
then checked against the configuration, and a mismatch is a warning too. The failure
this prevents is the one the September session had: a configuration that says one thing,
a run that does another, and nothing that says so.

## Smoke test of the graphed path on this machine

Worker started directly with `--low-latency --cuda-graph --talker-graph`, RTX 5070,
warm resident model, cached clone prompt:

| chars | generation | audio | RTF |
|---|---|---|---|
| 17 | 0.91 s | 2.32 s | 0.392 |
| 49 | 0.91 s | 2.72 s | 0.334 |
| 120 | 1.99 s | 6.48 s | 0.307 |

`backend=low_latency cuda_graph=1 talker_graph=1` on every request, and
`low_latency_detail` reported "predictor graph, talker graph". Consistent with Pass 4.
For comparison, request #61 of the live session generated 2.16 s of audio on the same
card in 17.8 s.

This is still TTS generation time, not conversational latency. It says the path works
here; it does not say what the user experiences.

## The scheduler passed over idle GPUs

`QwenTtsPool::AcquireWorker` chose the worker with the earliest predicted finish across
**busy and idle** workers, and then waited when the winner was busy. For one request in
isolation that is defensible. For a queue it is not: every waiting caller runs the same
comparison and reaches the same answer, so a long reply could park several phrases on
one card while the other sat idle. The session split 49 requests to the 2070 against 17
to the 5070, with queue depth reaching 8.

Selection now considers **only idle workers**, and waits only when every eligible worker
is busy — recomputing after each wake, never acting on a choice made before the wait.
The latency-critical pin to worker 0 became a preference: worker 0 wins while it is
free, and does not win while it is busy. The planner puts the newer chat GPU first on
the assumption that newer is faster, and the live data contradicts that under
contention — the 5070 also carries chat, and the session's very first phrase ran there
at RTF 6.44.

`SelectIdleVoiceWorker` is a free function so the choice is testable without two model
servers behind it; `Tests/voicePoolTests.cpp` covers faster-but-busy against
slower-but-idle, both idle, both busy, wake-and-recompute, and shutdown releasing a
blocked caller.

## Pool wait is now measured where it happens

`worker_queue_wait` was the worker's own Python lock wait, and it read 0.0 ms on all 66
requests of the session — structurally, because the pool picks a worker before that
worker ever sees the request. The real queue was in `AcquireWorker`, untimed, while
`queue_depth` climbed to 8.

Requests now carry `worker_pool_wait` (C++, entry to `AcquireWorker` until a worker is
held) alongside `python_lock_wait` (the old number, named for what it is).

## The 2070 VRAM growth was not a leak

Recorded explicitly so it is not reopened without contrary measurements.

Across the session the 2070's driver-reported usage climbed from 6702 MiB to 7538 MiB,
which looks like a leak. It is not. Every one of the 14 batch refusals reported
**exactly 2194 MiB available**, unchanged over 27 minutes. `available` is
`free + (reserved - allocated)`, so it stays constant precisely when live allocations
are flat and only PyTorch's caching arena grows: `free` falls by the same amount
`reusable` rises. A constant to the MiB across 27 minutes is the signature of the
allocator reserving and reusing, and it independently confirms Pass 5's "live
allocations are exactly flat".

Batching was refused on that card for a different and real reason — fp32 doubles the
projection, and the 1150 MiB reserve is 52% of the card's 2194 MiB headroom, leaving
room for roughly a 2-phrase, 28-character batch. That gate is deliberately **not** being
tuned yet. With graphs enabled the premise behind batching may not survive; Pass 5
already argues independent graphed phrases are the better route, and tuning a gate for a
path that may stop being the default is work spent ahead of the measurement.

## Still not verified

- **Live conversational time to first audible audio on the graphed path.** This is the
  question the next session exists to answer, and the one number that matters. The
  0.65 s figure elsewhere in this document is generation time for a short, already
  complete phrase; it is not user-to-audible latency and must not be quoted as one.
- **Live reply completion time on the graphed path**, against the 95–285 s baseline
  above.
- **Whether batching still earns its place** once phrases are graphed.
- Perceptual A/B voice quality, and live mid-batch interruption, both still open from
  Pass 5.
