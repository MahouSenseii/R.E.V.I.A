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
