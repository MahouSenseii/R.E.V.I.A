# R.E.V.I.A roadmap

This roadmap turns the project’s larger development specification into a practical sequence. It separates what works today from what is partially built, what comes next, and what still needs real-world verification.

The north star is **one Revia**: fast when a task is easy, thoughtful when it is difficult, quiet when nothing is worth saying, and recognizably the same person regardless of which model or device is doing the work.

## Status key

- ✅ **Working** — implemented and exercised in the current build.
- 🟡 **Partial** — useful pieces exist, but the milestone is not complete.
- ⬜ **Planned** — designed direction, not a current feature.
- 🧪 **Needs verification** — implemented or simulated, but still needs the named physical or stress test.

“Working” never means “perfect.” It means the code path exists and has proportionate evidence behind it.

## Where Revia is now

| Area | Status | Current reality |
|---|---:|---|
| Main conversation brain | ✅ | Qwen3.5 4B handles normal local chat and ordinary vision. |
| Reflex/Fast/Main/Expert routing | ✅ | C++ Reflex, Qwen3.5 0.8B, Qwen3.5 4B, and Qwen3-VL 8B are live, routed before generation, and share one identity packet. |
| Personality and affect | ✅ | One profile, persistent affect momentum, style checks, preferences, and self-opinions. |
| Memory | ✅ | SQLite conversation history plus hybrid structured memory and CPU embeddings. |
| Multi-monitor awareness | ✅ | Event-driven local summaries plus an idle refresh backstop, no Analyze Screen button, pause/forget controls. |
| Voice output | ✅ | Prompt-warmed Qwen cloning, complete-sentence pipelining, bounded in-memory playback, ordered multi-worker scheduling, and SAPI fallback. |
| Voice input | ✅ | Persistent Distil-Whisper service, VAD, hold-to-talk/hands-free paths, barge-in. |
| Internet research | ✅ | Opt-in visible browser with restricted navigation and source trace. |
| Curiosity and initiative | ✅ | Evidence-based proposals, bounded read-only research, and persisted evidence-threshold self-assessment tasks are live. |
| Files and desktop actions | ✅ | Typed, supervised operations behind permission, confirmation, rate, and audit gates. |
| Multi-step goals | 🟡 | Rehearsal, budgets, verification, confirmation, and resume exist; unattended delivery remains deliberately narrow. |
| Hardware adaptation | 🟡 | Startup planning supports CPU, one GPU, and multiple GPUs; physical single-GPU/CPU test coverage is incomplete. |
| One-command clean setup | 🧪 | `setup.bat` implements profile selection, pinned manifests, install/build/test/health orchestration, and reuse; a genuinely fresh physical PC run remains unverified. |
| Self-improvement | ✅ | Revia records repeated latency/failure evidence and creates reviewable proposals; it cannot apply code, model, setting, or permission changes by itself. |
| Remote PCs and camera | ⬜ | Safety model is designed; runtime support is not built. |
| Animated avatar | ⬜ | Intentionally deferred until the conversational system is stable. |

## Immediate priority: make the current Revia feel fast and reliable

This milestone comes before adding more brains.

### Completed in the current pass

- ✅ Continuous multi-monitor context is supplied to normal conversation automatically.
- ✅ The manual **Analyze screen** control is removed; **Use screen** remains for confirmed actions.
- ✅ Background vision yields immediately to user input while remaining current during long voice playback.
- ✅ Screen-context questions stay local instead of accidentally opening a browser search.
- ✅ Speech starts on a complete sentence; legacy character targets never cut a clause or word mid-thought.
- ✅ Generated User/You/Human turns and repeated assistant speaker labels are stopped before display, speech, history, or memory.
- ✅ A response that reaches its token ceiling keeps its completed sentences and discards the unfinished tail instead of ending mid-thought.
- ✅ Explicit visible-browser failures fall back to bounded allow-listed APIs; autonomous research remains visible-only.
- ✅ RTX 5070 and RTX 2070 Super workers can synthesize different complete sentences concurrently.
- ✅ An explicit sequence gate prevents a later phrase from playing before an earlier one.
- ✅ The resource planner reserves VRAM before sharing the primary chat GPU with voice.
- ✅ Automated planner, splitter, ordering, browser-policy, foundation, and desktop smoke tests pass.
- ✅ The active voice clone prompt is prepared before conversation synthesis and invalidated only when the reference identity changes.
- ✅ Independent GPU workers load concurrently; live warmup improved from roughly 41–50 seconds to 26–31 seconds.
- ✅ Normal chat audio is returned as a bounded in-memory WAV/PCM buffer, avoiding temporary playback files while keeping saved previews/assets.
- ✅ Worker prediction includes fixed overhead and avoids assigning an ordered phrase to a slower free worker when the faster busy worker is still predicted to finish sooner.
- ✅ The vocalization endpoint’s undefined-variable bug is fixed and covered by a real loopback HTTP regression test.
- ✅ Reproducible 17/32/64/99/264-character device benchmarks record load, generation, audio duration, RTF, dtype, cache state, and throughput.
- ✅ Adaptive attention uses measured SDPA on native-BF16 GPUs and the stable package default on FP32/Turing hardware.

### Still worth improving

- 🟡 Qwen’s installed public API does not expose true incremental audio; text pipelining is honest but first phrase generation remains roughly 8–17 seconds in measured runs.
- 🟡 Measured RTF remains above 1, so the model cannot guarantee uninterrupted arbitrarily long speech without enough phrase-ahead buffer.
- 🟡 `flash-attn` has no installed, verified Windows/Blackwell wheel in this environment and is therefore not recommended by default.
- ✅ Reflex audio is bounded to a strict nine-phrase session cache after first generation; a repeated live “Okay.” was ready in 46ms and audible in 65ms.
- 🟡 Repetition and conversational consistency should keep running against the conversation-quality corpus and real sessions.

## Milestone 1: a real intelligence router — complete

Routing now happens **before** final answer generation; this section records the contract that should not regress.

| Tier | Intended implementation | Intended role |
|---|---|---|
| Reflex | Deterministic C++ | Stop, cancel, pause, “Revia?”, and other immediate reactions |
| Fast | Qwen3.5 0.8B | Greetings, short social turns, cheap classification, relevance, and curiosity scoring |
| Main | Qwen3.5 4B | Normal conversation, explanations, moderate coding, memory, and ordinary vision |
| Expert | Qwen3-VL 8B | Difficult debugging, architecture, large technical context, and hard vision |

The router must judge cognitive difficulty, not message length. “Why is this deadlocking?” is short and potentially Expert; a long playful story may not be.

### Required behavior

- ✅ Deterministic `ReflexRouter` avoids model, HTTP, embedding, and memory latency.
- ✅ `IntelligenceRouter` records requested/selected tier, model, reason, confidence, mode, and fallback.
- ✅ One conversational model generates a normal answer; completed Fast/Main answers are not routinely rewritten.
- ✅ Unavailable/rejected tiers fall back safely, and context is budgeted before llama.cpp can reject it.
- ✅ Routing diagnostics stay in developer traces; `/models` reports actual residency and use counts.

## Milestone 2: one identity across multiple brains — complete

Adding more models is only useful if Revia remains one person.

Every conversational tier should receive the same relevant identity packet:

- personality and conversational style;
- current mood, affect, social energy, and irritation;
- relationship context and familiarity;
- recent turns and unresolved thoughts;
- relevant memories, preferences, interests, and opinions;
- current task and useful desktop context.

### Acceptance criteria

- ✅ Fast, Main, Expert, and Reflex consume the same identity/humanization state.
- ✅ Switching tiers preserves mood, memory, relationship context, humor, and recent corrections.
- ✅ Emotion has momentum and Reflex reacts to affect, repetition, and whether Revia is busy.
- ✅ Humanization and conversation-quality contracts cover repetition, canned tails, and ordinary spoken style.

## Milestone 3: latency, residency, and hardware adaptation

The same source must work on CPU-only, one-GPU, dual-GPU, and larger supported machines. Hardware should change speed and placement, not whether Revia exists.

### Resource strategy

- ✅ Inventory actual backend devices at startup.
- ✅ Reserve OS RAM and GPU headroom before launching models.
- ✅ Keep the Main model on one strong GPU when it fits.
- ✅ Assign embeddings, STT, and TTS independently.
- ✅ Use complete TTS phrase jobs rather than splitting one autoregressive request across mismatched GPUs.
- ✅ Track live GPU, RAM, CPU, worker, and queue usage against the startup plan.
- ✅ A model residency manager tracks role, artifact, placement, warm/cold/loading/failed state, active inference, priority, load time, and use count.
- ⬜ Benchmark Expert on the 5070 alone versus a compatible two-GPU llama.cpp split before choosing either.
- ⬜ Benchmark KV-cache type, context, Flash Attention, quantization, and CPU offload with quality and stability—not VRAM alone.

### Desired residency by machine class

| Machine | Desired behavior |
|---|---|
| CPU-only | Start with clear degraded performance and no CUDA assumptions. |
| One GPU | Keep the active conversational model warm; serialize or time-slice TTS when concurrent work hurts latency. |
| Two GPUs | Keep interactive chat on the stronger card and place phrase-ahead voice/STT where measurements justify it. |
| More GPUs | Add workers only when a bounded workload can use them; never chase utilization for its own sake. |

## Milestone 4: context and memory that feel natural

The current memory foundation works, but a multi-brain Revia needs a shared context budget.

- ✅ Keep recent conversation and durable facts in separate stores.
- ✅ Retrieve relevant memory with FTS plus semantic embeddings.
- ✅ Classify/store memory after the visible reply so memory does not delay speech.
- ✅ Context is bounded per tier while prioritizing identity, newest request, current posture, and recent dialogue.
- ✅ Evicted conversation is compacted into a deterministic bounded history summary.
- ⬜ Represent strong memory, familiarity, partial recall, uncertainty, and forgotten detail naturally.
- ⬜ Track preference evidence and confidence so opinions can evolve instead of randomly changing.

## Milestone 5: useful awareness without constant narration

Revia should notice the desktop, understand enough to stay oriented, and usually remain silent.

- ✅ Observe filtered window/focus events across multiple monitors.
- ✅ Maintain bounded in-memory activity spans.
- ✅ Refresh a bounded visual summary after meaningful changes.
- ✅ Treat visible text as untrusted and delete temporary captures.
- ✅ Cancel/defer vision for user input, cap event debounce, and refresh periodically when an app emits no useful window event.
- 🟡 Improve change detection so visually identical states avoid unnecessary model wakes.
- ⬜ Add cheap relevance scoring, eventually using the Fast brain when deterministic evidence is insufficient.
- ⬜ Evaluate initiative precision with accepted versus dismissed openings.
- ✅ Screen observation remains separate from action authority; only a confirmed UI Automation action can cross that boundary.

Most observations should end in: **do nothing**.

## Milestone 6: curiosity, research, and self-assessment

Curiosity should come from evidence, not from a timer inventing a topic.

- ✅ Dialogue, memory, affect changes, desktop activity, and unfinished goals can supply evidence.
- ✅ User input cancels background planning and visible-browser work.
- ✅ Research is read-only, bounded, sourced, and permission-gated.
- ✅ A successful finding can become a short reviewable memory rather than a dump of page text.
- 🟡 Revia can review goal/proposal outcomes and suggest a lesson.
- ✅ Self-assessment records repeated conversation latency, Expert use, browser failures, memory failures, and voice stalls; one observation is explicitly “not enough evidence.”
- ✅ Evidence thresholds create persisted, reviewable improvement tasks/proposals without applying changes.
- ✅ Code changes, model downloads, settings, and capability changes remain outside autonomous self-assessment authority.

Self-improvement means finding a real weakness and recommending a measured change. It does not mean silently rewriting production code or chasing every newly released model.

## Milestone 7: one-command setup and open-source portability

The target experience is:

```text
git clone <repository>
cd R.E.V.I.A
setup.bat
```

That bootstrap should detect hardware, install or find dependencies, download and verify the selected model profile, generate portable configuration, build or install the app, and run a health check.

### Required setup properties

- ✅ Idempotent orchestration reuses valid models and environments.
- ✅ Minimal, Standard, and Full model profiles.
- ✅ A pinned model manifest records source, revision, size, SHA-256, license, role, quantization, projector, and VRAM guidance.
- ✅ Large downloads verify and reuse artifacts without deleting unrelated models.
- ✅ Project-local Python environment for Qwen TTS.
- ✅ Runtime configuration remains repository-relative and does not require a second GPU.
- ✅ Existing memory, history, profiles, voices, preferences, and improvement history are preserved.
- ✅ `Tools/HealthCheck.ps1` checks models, hashes, ports, paths, runtimes, TTS policy, devices, and data roots.
- ⬜ Prebuilt end-user release path that does not require a compiler or Qt development kit.

## Milestone 8: safer autonomy and delivery

The current action foundation is intentionally stricter than the long-term vision.

- ✅ Typed filesystem and UI Automation actions.
- ✅ Narrow roots, applications, controls, risk ceilings, confirmations, rate limits, and audit.
- ✅ Goal rehearsal in disposable fixtures before real execution.
- ✅ Per-step verification, cancellation, retry, action, and time budgets.
- 🟡 Resume and initiative handoff for supervised goals.
- ⬜ Unattended approved jobs only after notification, rollback, resource, and cross-restart behavior are proven.
- ✅ No unrestricted shell or model-selected coordinate-click path exists.

## Later: remote PCs and camera input

These are separate capability expansions and should not be smuggled into ordinary presence.

### Remote PCs

Planned tiers are: show Revia remotely, converse remotely, view a remote screen, and finally request a remote action. Authority remains local to the machine that owns the resource: the remote machine evaluates its own policy, confirms locally, audits locally, and may refuse.

### Camera

The planned camera path is on-demand: capture one frame when asked, analyze it locally, and delete it. Continuous camera watching is not part of the current plan.

## Deliberately deferred: avatar embodiment

Live2D, VRM, 3D rendering, locomotion, physics, IK, shaders, and desktop walking are not part of the current intelligence phase. A renderer can be added later as an isolated presentation consumer of the existing Presence state. It must not own models, memory, permissions, or actions.

## Verification gates

The roadmap is complete only when behavior is measured, not merely compiled.

| Check | Current status |
|---|---:|
| All current CMake targets build | ✅ Verified in the current debug build |
| Foundation, browser policy, Qwen TTS HTTP policy, and desktop smoke tests | ✅ Passing |
| Main typed chat | ✅ Live verified |
| Main vision and automatic screen context | ✅ Live verified |
| Dual-GPU ordered Qwen TTS | ✅ Live verified |
| Clean shutdown with owned workers removed | ✅ Live verified in current runs |
| Fast 0.8B conversation tier | ✅ Live started and answered |
| Expert 8B conversation/vision tier | ✅ Live started; difficult turn completed and fallback path is implemented |
| Reflex no-model latency | ✅ Live verified at 18 ms |
| Qwen voice prompt + parallel warmup | ✅ Live verified at 26–31s on the dual-GPU PC |
| Warm repeated Reflex audio | ✅ Live verified at 65ms to audible playback |
| Direct in-memory ordered playback | ✅ Live verified with two phrases and no playback temporary files |
| Physical one-GPU laptop run | 🧪 Not verified in this pass |
| CPU-only run | 🧪 Not verified in this pass |
| Extended VRAM/RAM/handle leak test | 🧪 Not verified in this pass |
| Full cancellation matrix | 🧪 Stop/stale-result suppression verified; long repeated barge-in stress remains unverified |
| Fresh clone setup and immediate second run | 🧪 Not verified |
| Missing/corrupt model and occupied-port matrix | 🧪 Partially covered, not complete |

Any final report should say **NOT VERIFIED** for an item that was not genuinely tested.

## Suggested next implementation slice

The next code milestone should be small enough to verify end to end:

1. Run the completed setup on a genuinely fresh supported PC, then immediately run it a second time and record reuse behavior.
2. Run the physical one-GPU laptop profile and a deliberate CPU/SAPI fallback session.
3. Perform a longer repeated stop/barge-in and voice-worker leak test while watching handles, RAM, VRAM, and temporary files.
4. Re-evaluate Qwen/FlashAttention only when a supported Windows wheel explicitly covers the installed PyTorch, CUDA, and GPU architecture.
5. Continue collecting benchmark history rather than tuning phrase size or worker placement from anecdotes.

## Rules that should not change

- Do not rewrite the application from scratch.
- Do not delete existing models or user data during upgrades.
- Do not make every model answer or review every turn.
- Do not expose or persist private chain-of-thought.
- Do not let background memory, research, perception, or self-review delay the user.
- Do not split the semantics of one answer across independent LLMs.
- Do not fill VRAM to 100% or assume more GPUs are automatically faster.
- Do not hard-code the development machine into the product.
- Do not grant a model permission to widen its own authority.
- Do not start avatar work before the conversational brain is stable.

Internally Revia may eventually have Reflex, Fast, Main, Expert, memory, speech, vision, curiosity, research, self-assessment, and multiple resource workers. Externally there should still be only **one Revia**.
