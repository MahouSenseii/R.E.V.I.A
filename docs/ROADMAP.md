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
| Main conversation brain | ✅ | Qwen3.5 4B handles normal local chat and vision. |
| Fast/Main/Expert routing | ⬜ | The 0.8B and 8B model files exist, but they are not yet live conversation tiers. |
| Personality and affect | ✅ | One profile, persistent affect momentum, style checks, preferences, and self-opinions. |
| Memory | ✅ | SQLite conversation history plus hybrid structured memory and CPU embeddings. |
| Multi-monitor awareness | ✅ | Event-driven local summaries, no Analyze Screen button, pause/forget controls. |
| Voice output | ✅ | Qwen voice cloning, bounded phrase streaming, two-GPU generation, ordered playback, SAPI fallback. |
| Voice input | ✅ | Persistent Distil-Whisper service, VAD, hold-to-talk/hands-free paths, barge-in. |
| Internet research | ✅ | Opt-in visible browser with restricted navigation and source trace. |
| Curiosity and initiative | 🟡 | Evidence-based proposals and bounded read-only research exist; deeper self-assessment is still limited. |
| Files and desktop actions | ✅ | Typed, supervised operations behind permission, confirmation, rate, and audit gates. |
| Multi-step goals | 🟡 | Rehearsal, budgets, verification, confirmation, and resume exist; unattended delivery remains deliberately narrow. |
| Hardware adaptation | 🟡 | Startup planning supports CPU, one GPU, and multiple GPUs; physical single-GPU/CPU test coverage is incomplete. |
| One-command clean setup | ⬜ | Component installers exist, but one fully verified idempotent bootstrap does not. |
| Self-improvement | 🟡 | Revia can review outcomes and propose memories; evidence-backed code/model improvement proposals are not complete. |
| Remote PCs and camera | ⬜ | Safety model is designed; runtime support is not built. |
| Animated avatar | ⬜ | Intentionally deferred until the conversational system is stable. |

## Immediate priority: make the current Revia feel fast and reliable

This milestone comes before adding more brains.

### Completed in the current pass

- ✅ Continuous multi-monitor context is supplied to normal conversation automatically.
- ✅ The manual **Analyze screen** control is removed; **Use screen** remains for confirmed actions.
- ✅ Background vision yields to user input and pending voice work.
- ✅ Screen-context questions stay local instead of accidentally opening a browser search.
- ✅ Long replies split at sentence, clause, or word boundaries around a 64-character target.
- ✅ RTX 5070 and RTX 2070 Super workers can synthesize different phrases concurrently.
- ✅ An explicit sequence gate prevents a later phrase from playing before an earlier one.
- ✅ The resource planner reserves VRAM before sharing the primary chat GPU with voice.
- ✅ Automated planner, splitter, ordering, browser-policy, foundation, and desktop smoke tests pass.
- ✅ A live four-sentence sample reduced first-phrase synthesis from 45.4 seconds to 18.1 seconds, with later phrases completing in roughly 9–20 seconds.

### Still worth improving

- 🟡 Qwen TTS warmup remains about 34–41 seconds after startup.
- 🟡 First audio is much better but still not conversationally instant.
- 🟡 `flash-attn` is unavailable in the current Windows Qwen TTS environment.
- 🟡 Speech timing needs explicit `first_audio_ready` and `first_audio_played` metrics instead of inferring them from phrase-generation events.
- 🟡 Repetition and conversational consistency should keep running against the conversation-quality corpus and real sessions.

## Milestone 1: a real intelligence router

The next major architecture step is routing **before** final answer generation.

| Tier | Intended implementation | Intended role |
|---|---|---|
| Reflex | Deterministic C++ | Stop, cancel, pause, “Revia?”, and other immediate reactions |
| Fast | Qwen3.5 0.8B | Greetings, short social turns, cheap classification, relevance, and curiosity scoring |
| Main | Qwen3.5 4B | Normal conversation, explanations, moderate coding, memory, and ordinary vision |
| Expert | Qwen3-VL 8B | Difficult debugging, architecture, large technical context, and hard vision |

The router must judge cognitive difficulty, not message length. “Why is this deadlocking?” is short and potentially Expert; a long playful story may not be.

### Required behavior

- ⬜ Build a deterministic `ReflexRouter` for reactions that should not pay HTTP, embedding, SQLite, Python, or LLM latency.
- ⬜ Build an `IntelligenceRouter` that records requested tier, selected tier, model, reason, confidence, and reasoning mode.
- ⬜ Use only one conversational model for a normal turn.
- ⬜ Allow an early cancel-and-escalate when the chosen model is clearly insufficient.
- ⬜ Never finish a Main answer and then routinely ask Expert to rewrite it.
- ⬜ Keep routing diagnostics visible to developers but invisible in ordinary conversation.
- ⬜ Prove actual model usage with worker logs and timing events rather than assuming the configured path ran.

## Milestone 2: one identity across multiple brains

Adding more models is only useful if Revia remains one person.

Every conversational tier should receive the same relevant identity packet:

- personality and conversational style;
- current mood, affect, social energy, and irritation;
- relationship context and familiarity;
- recent turns and unresolved thoughts;
- relevant memories, preferences, interests, and opinions;
- current task and useful desktop context.

### Acceptance criteria

- ⬜ Fast, Main, and Expert answers sound like the same person using different amounts of effort.
- ⬜ Switching tiers does not reset mood, memory, relationship, humor, or vocabulary.
- ⬜ Emotion has inertia and decays gradually rather than changing randomly per fragment.
- ⬜ Reflex replies respond to affect and recent repetition without being a context-free random phrase list.
- ⬜ Revia can hesitate, clarify, correct herself, or change her mind without becoming unsafe or factually careless.
- ⬜ Ordinary spoken chat does not default to headings, executive summaries, or “How can I help?” tails.

## Milestone 3: latency, residency, and hardware adaptation

The same source must work on CPU-only, one-GPU, dual-GPU, and larger supported machines. Hardware should change speed and placement, not whether Revia exists.

### Resource strategy

- ✅ Inventory actual backend devices at startup.
- ✅ Reserve OS RAM and GPU headroom before launching models.
- ✅ Keep the Main model on one strong GPU when it fits.
- ✅ Assign embeddings, STT, and TTS independently.
- ✅ Use complete TTS phrase jobs rather than splitting one autoregressive request across mismatched GPUs.
- 🟡 Track live GPU, RAM, CPU, worker, and queue usage against the startup plan.
- ⬜ Add a model residency manager that knows which brain is warm, cold, loading, busy, or safe to unload.
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
- ⬜ Add a clear context budget for identity, humanization state, recent turns, retrieved memories, current task, and desktop context.
- ⬜ Compress older conversation into a compact history instead of injecting everything.
- ⬜ Represent strong memory, familiarity, partial recall, uncertainty, and forgotten detail naturally.
- ⬜ Track preference evidence and confidence so opinions can evolve instead of randomly changing.

## Milestone 5: useful awareness without constant narration

Revia should notice the desktop, understand enough to stay oriented, and usually remain silent.

- ✅ Observe filtered window/focus events across multiple monitors.
- ✅ Maintain bounded in-memory activity spans.
- ✅ Refresh a bounded visual summary after meaningful changes.
- ✅ Treat visible text as untrusted and delete temporary captures.
- ✅ Cancel/defer vision for user input and speech.
- 🟡 Improve change detection so visually identical states avoid unnecessary model wakes.
- ⬜ Add cheap relevance scoring, eventually using the Fast brain when deterministic evidence is insufficient.
- ⬜ Evaluate initiative precision with accepted versus dismissed openings.
- ⬜ Keep screen observation separate from action authority permanently.

Most observations should end in: **do nothing**.

## Milestone 6: curiosity, research, and self-assessment

Curiosity should come from evidence, not from a timer inventing a topic.

- ✅ Dialogue, memory, affect changes, desktop activity, and unfinished goals can supply evidence.
- ✅ User input cancels background planning and visible-browser work.
- ✅ Research is read-only, bounded, sourced, and permission-gated.
- ✅ A successful finding can become a short reviewable memory rather than a dump of page text.
- 🟡 Revia can review goal/proposal outcomes and suggest a lesson.
- ⬜ Add self-assessment metrics for routing mistakes, repeated response failures, latency, queue stalls, and unnecessary Expert use.
- ⬜ Let Revia research an observed weakness using primary sources and produce an evidence-backed improvement proposal.
- ⬜ Keep code changes, model downloads, and capability changes behind explicit user approval, build/test review, and rollback information.

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

- ⬜ Idempotent second run: valid models and environments are reused.
- ⬜ Minimal, Standard, and Full model profiles.
- ⬜ A model manifest with source, revision, size, checksum, license, role, quantization, projector relationship, and recommended VRAM.
- ⬜ Resume and verify large downloads without deleting unrelated models.
- ⬜ Project-local Python environment for Qwen TTS.
- ⬜ No hard-coded username, checkout path, drive letter, Python path, CUDA path, or second GPU.
- ⬜ Preserve memory, conversation history, profiles, voices, preferences, and improvement history across updates.
- ⬜ `revia --health-check` for models, configuration, ports, SQLite, devices, and services.
- ⬜ Prebuilt end-user release path that does not require a compiler or Qt development kit.

## Milestone 8: safer autonomy and delivery

The current action foundation is intentionally stricter than the long-term vision.

- ✅ Typed filesystem and UI Automation actions.
- ✅ Narrow roots, applications, controls, risk ceilings, confirmations, rate limits, and audit.
- ✅ Goal rehearsal in disposable fixtures before real execution.
- ✅ Per-step verification, cancellation, retry, action, and time budgets.
- 🟡 Resume and initiative handoff for supervised goals.
- ⬜ Unattended approved jobs only after notification, rollback, resource, and cross-restart behavior are proven.
- ⬜ Never add unrestricted shell or model-selected coordinate clicks as a shortcut.

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
| Foundation, browser policy, and desktop smoke tests | ✅ Passing |
| Main typed chat | ✅ Live verified |
| Main vision and automatic screen context | ✅ Live verified |
| Dual-GPU ordered Qwen TTS | ✅ Live verified |
| Clean shutdown with owned workers removed | ✅ Live verified in current runs |
| Fast 0.8B conversation tier | ⬜ Not implemented |
| Expert 8B conversation/vision tier | ⬜ Not implemented |
| Repeated model-switch stress | ⬜ Not applicable yet |
| Physical one-GPU laptop run | 🧪 Not verified in this pass |
| CPU-only run | 🧪 Not verified in this pass |
| Extended VRAM/RAM/handle leak test | 🧪 Not verified in this pass |
| Full cancellation matrix | 🧪 Partially covered, not fully stress-tested |
| Fresh clone setup and immediate second run | 🧪 Not verified |
| Missing/corrupt model and occupied-port matrix | 🧪 Partially covered, not complete |

Any final report should say **NOT VERIFIED** for an item that was not genuinely tested.

## Suggested next implementation slice

The next code milestone should be small enough to verify end to end:

1. Add the routing decision/event types and latency fields without changing the active 4B path.
2. Implement deterministic Reflex handling for stop, cancel, pause, quiet, and “Revia?”.
3. Add a 0.8B Fast worker behind a feature flag.
4. Route a narrow, testable set of casual turns to Fast while keeping one shared identity/context builder.
5. Benchmark TTFT, response quality, VRAM, cancellation, and unnecessary escalation.
6. Keep the feature off by default until routing and humanization audits pass.

Only after that slice is stable should the 8B Expert worker enter the live routing path.

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
