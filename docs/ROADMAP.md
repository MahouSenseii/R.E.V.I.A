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
| Animated avatar | 🟡 | Canonical character/palette and a bounded presence bridge exist; a real Live2D/VRM renderer and model are not selected or live-verified. |

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

## Active foundation: avatar embodiment

The character design, palette, expression map, bounded Presence stream, and public conversation boundary are established. A real Live2D/VRM model, renderer adapter, OBS routing, and platform connectors still require explicit provider selection, credentials, assets, and live verification. Rendering remains an isolated presentation consumer and must not own models, memory, permissions, or actions. Locomotion, physics, IK, shaders, and desktop walking remain later presentation work.

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
- Keep avatar work behind the replaceable Presence boundary; rendering never owns Revia's mind or authority.

Internally Revia may eventually have Reflex, Fast, Main, Expert, memory, speech, vision, curiosity, research, self-assessment, and multiple resource workers. Externally there should still be only **one Revia**.

## Persistent mind architecture (in progress)

The goal is that Revia's emotions, personality, and relationships are runtime-owned
state that the conversational model receives, rather than something the model invents
per turn.

**Phase 1 — state foundations: implemented and tested.**

- `Public/Emotion/emotionTypes.h` — 20-component `EmotionVector`. Emotions coexist;
  a dominant reading is derived for UI and logging without collapsing the rest.
- `Public/Emotion/stimulus.h` — typed `Stimulus` with valence, importance, novelty,
  certainty, success/failure, and causation. Nothing downstream branches on prose.
- `Public/Emotion/moodState.h` — `MoodState` plus `MoodController`: momentum, decay,
  saturation, and `AppraisalGain`, which is how mood changes what the next event feels
  like rather than merely being reported.
- `Public/Identity/developmentState.h` — 16 traits as base + learned delta, never
  collapsed, so drift is explainable and reversible. `ChildlikeBaseline()` encodes her
  documented starting temperament as numbers.
- `Public/Identity/relationshipState.h` — per-entity relationship with bounded evidence
  application. Affinity starts neutral: nothing assumes she likes anyone.
- `Public/Identity/identityStore.h` — atomic, schema-versioned persistence of
  development, mood, relationships, and development history. A corrupt or newer-schema
  file is refused rather than silently replaced.

**Phase 2 — appraisal: implemented and tested.**

- `Public/Emotion/appraisalContext.h` — derives the appraisal axes (expectedness, goal
  relevance, novelty, controllability, self-responsibility, social importance) from a
  stimulus plus current state. Retrieved memories are bounded to six and temper how
  surprising a familiar outcome is.
- `Public/Emotion/emotionModel.h` — `IEmotionModel` with `RuleEmotionModel` behind it.
  The rule model is permanent: the fallback when no network is loaded, the source of
  initial training targets, and the baseline a trained model must beat. Personality is
  applied as a separate multiplier after the appraisal arithmetic, never folded into
  coefficients, and `RawResponse` exposes the pre-personality delta so that influence
  can be measured rather than asserted.
- `Public/Emotion/emotionRuntime.h` — owns the emotion vector and mood, applies a model
  to a stimulus, integrates the result, and records an `AppraisalOutcome` carrying the
  stimulus, context, delta, resulting state, and model name.
- `EmotionRuntime::ToAffectSnapshot()` projects the vector onto the legacy
  `AffectSnapshot`, so the badge, speech rate, and posture line can be migrated
  incrementally and `AffectController` stays a working fallback rather than dead code.

Observed behaviour from the same stimulus under different context:

| Event | Result |
| --- | --- |
| Goal failed, her approach | frustration 0.54, disappointment 0.41, confusion 0.38 |
| Goal failed, external cause | concern 0.43, disappointment 0.41, confusion 0.38 |
| Hard-won success | joy 0.66, pride 0.57, excitement 0.54 |
| Sharp remark from a close friend | amusement 0.38, irritation 0.08 |
| The same remark from a stranger | irritation 0.32, anger 0.24 |
| The same remark from a friend, on a bad day | sadness 0.29, disappointment 0.24 |

**Phase 3 — dynamic identity: implemented, integrated, and tested.**

- `Public/Identity/reviaStatePacket.h` — the single canonical description of Revia
  handed to whichever model answers. `RenderStatePacket` assembles the documented
  section order and is deterministic, which is what guarantees Reflex, Fast, Main, and
  Expert cannot be given different descriptions of the same moment.
- Sections render only when they carry something real. A development section with no
  drift, or a relationship section for a stranger, would assert state that does not
  exist, so absence is the honest rendering.
- `Private/Runtime/conversationRuntime.cpp` now assembles the packet instead of
  concatenating the prompt inline. The emotion vector is populated from the
  deterministic `AffectController` via `LegacyAffectToVector`, so behaviour is unchanged
  while the assembly moves; when appraisal goes live only that population changes.
- The prompt-leak filter was extended to cover the new sections. It also had a real
  pre-existing bug: the marker `"runtime self-knowledge (ground truth)"` ended in a
  parenthesis the rendered prompt never contains (`"(ground truth; mention it only..."`),
  so that marker had never matched anything. Fixed.

**Phase 4 — relationships: implemented, integrated, and tested.**

- `Public/Identity/relationshipRegistry.h` — the live per-entity database, loaded at
  startup and saved on shutdown. Entity ids are namespaced (`local:user`,
  `adapter:discord:name`) so two people who share a name stay two relationships, and
  adapter-supplied author strings are sanitised before they key anything.
- `Public/Identity/relationshipEvidence.h` — deterministic signal reading. A model never
  assigns relationship numbers; evidence comes from what the runtime can observe, so
  claiming to be trusted produces no trust. Hostility aimed at Revia accrues as
  grievance; frustration at a broken tool does not; being corrected is friction rather
  than disrespect.
- `ReviaSession` resolves the speaker, applies evidence after each turn, and supplies the
  relationship to the state packet through a provider, so the relationship section now
  renders for real in conversation.
- Warmth is capped by acquaintance (`0.25 + 0.75 * familiarity`). Without it affinity and
  trust saturated in about thirty exchanges while familiarity needed hundreds, and the
  rendered sentence contradicted itself: *"nearly a stranger to you, you like them, you
  trust them"*. The cap is one-directional -- dislike stays fast, because deciding you
  want nothing to do with someone does not require history.

Observed accumulation against sustained appreciation, and against hostility:

| Exchanges | Rendered |
| --- | --- |
| 30 | they are nearly a stranger to you. |
| 150 | you are still getting to know them, and you like them. |
| 400 | you know them well, you like them, you trust them, and you look up to them. |
| 25 hostile | nearly a stranger, you do not like them, ... annoyed with them right now. |

**Phase 5 — development, and appraisal going live: implemented and tested.**

- `Public/Emotion/stimulusBuilder.h` — one place where stimuli are constructed from
  confirmed outcomes. Conversation stimuli reuse the same signals the relationship system
  reads, so what moves a feeling and what moves a relationship cannot disagree about what
  was said.
- **Appraisal is now the live emotion path.** `ConversationRuntime` builds a stimulus
  before generation, `EmotionRuntime` appraises it against development, mood, and the
  speaker's relationship, and the resulting vector is what reaches the prompt, the status
  badge, and speech. `AffectController` still runs as the documented deterministic
  fallback and baseline, and `LegacyAffectToVector` covers paths with no stimulus behind
  them, such as a proactive opening.
- `Public/Identity/developmentEngine.h` — evidence accumulates per trait and only moves a
  personality when several consistent observations agree. Changes are bounded per step,
  capped over a lifetime, reversible when evidence changes direction, and carry the reason
  and evidence count that produced them.
- Development has no preferred direction. Impulsiveness that keeps paying off raises
  impulsiveness and risk tolerance; impulsiveness that keeps failing lowers it and raises
  caution. Growth is not a slide toward a calmer, more agreeable assistant.
- Mood is restored at startup and captured at shutdown. Momentary emotion deliberately is
  not: waking up annoyed about something she cannot point at would be worse than waking
  up calm.

Verified live, not only in tests. A full `--runtime-ready-smoke-test` cycle logs
`Identity loaded: 0 known relationship(s).`, records `identity_load=0.6ms` in the startup
timings, and logs `Identity saved` during shutdown, leaving a schema-version-1
`RuntimeData/Identity/identity.json` with all sixteen traits persisted by name.

**Not yet implemented.**

- The packet's `memories` field is never populated. Memory does still reach the prompt:
  `promptBuilder::BuildMessages` performs its own BM25/vector retrieval and inserts a
  memory block directly. Consolidating that into the packet is outstanding, as are the
  autobiographical metadata fields (`emotionAtEncoding`, `relationshipAtEncoding`) and
  memory strength/decay.
**Debug UI: implemented.** `Desktop/mindPanel.cpp` adds a Mind tab with three views --
Now (the full emotion vector including the zeroes, plus mood), Development (base, change,
and current side by side, with the applied-change history and the evidence behind each),
and Relationships (every known entity with familiarity, affinity, trust, respect,
irritation, resentment, and exchange count). Read-only: a control that could set trust to
0.9 would be exactly the assignment path the relationship system refuses the model.

**Verified against a live conversation turn**, not only in tests. Piping one message
through the CLI produced: identity loaded at startup, a reply, a `local:user`
relationship at familiarity 0.0023 / affinity +0.0269 / trust 0.2517 after one exchange,
mood valence nudged to +0.008 and sociability 0.600 to 0.605, an empty development
history (one observation being far below the four-observation threshold), and
`Identity saved: 1 relationship(s)` on shutdown.
**Phase 6 — drives and the activity scheduler: implemented and tested, not yet running.**

- `Public/Autonomy/driveState.h` — seven drives with their own dynamics. Everything
  decays except boredom, which is what nothing happening feels like; acting on a drive
  spends it, so finding an answer actually reduces the wanting.
- `Public/Autonomy/activity.h` — activity types and lifecycle. `Interrupted` is distinct
  from `Paused` and from `Cancelled`, because an interruption is not a decision and what
  the user cut off deserves a chance to continue.
- `Public/Autonomy/activityScheduler.h` — evidence-gated scoring. `Nothing` is a
  first-class candidate scored exactly at the bar, so any real activity has to beat it
  outright rather than win by default.

The load-bearing property, tested directly: **a timer alone can never produce an
activity.** Maxed-out drives with no evidence return `Nothing`. Over sixty-four ordinary
idle evaluations fewer than a quarter produce any activity at all.

An active conversation is a *hard gate*, not a score penalty. It was written as a penalty
first, and a strong enough drive could outbid the user's attention -- exactly backwards.
Missing permission is likewise refused by name rather than being silently absent, so
"why did she not look it up?" has an answer.

Observed decisions:

| Situation | Decision |
| --- | --- |
| Idle, nothing happened | nothing -- "Nothing has happened that would justify doing anything." |
| Unfinished important goal | continue goal (1.14) |
| The same, mid-conversation | nothing -- "A conversation is in progress; nothing autonomous outranks that." |
| Open question, research allowed | research (0.78) |
| Open question, research off | nothing -- "no permission to research it" |
| Worth saying, long quiet | speak (0.55, marginal) |
| Worth saying, just talked | nothing -- "the user was interacting too recently" |

**Wired into the runtime.** `ReviaSession` owns the drive state and scheduler. Drives
move from the same stimuli the appraisal sees (conversation via a stimulus observer, goal
outcomes directly), settle toward baseline on each initiative signal, and are spent when
an activity acts on them. `ConsiderAutonomousActivity` runs from the initiative worker,
`ContinueGoal` executes through the ordinary goal runner, and user input calls
`PreemptAutonomousActivity`, which marks the activity `Interrupted` rather than
`Cancelled` so it stays resumable.

Only `ContinueGoal` executes. Every other decided activity is recorded and published but
explicitly reported as not yet carried out, rather than silently no-opping into something
that looks like a working feature.

The scheduler is deliberately not given authority to speak: `openQuestion` and
`somethingWorthSaying` are left unpopulated in `GatherAutonomyEvidence` because the
existing curiosity and initiative controllers already own that channel, and two systems
with the same authority over speech would produce two unprompted lines at once.

Placement mattered and was wrong at first. The call originally sat after the initiative
loop's microphone and busy checks. Those guard the interruption point -- speaking over
someone -- and hands-free listening keeps the microphone recording continuously, so
gating on it meant resuming her own unfinished work was permanently suppressed by a
microphone she was not going to use. It now runs before those checks and relies on the
scheduler's own finer-grained gates, where an active conversation is a hard refusal.

**Verified running.** `ReviaDesktop.exe --runtime-hold <seconds>` starts the runtime,
stays alive past the initiative debounce, and shuts down cleanly, which is what makes
this checkable at all -- `--runtime-ready-smoke-test` exits the moment startup reports
ready, before any background worker has done anything. A seventy-second hold produced:

```
Initiative woke: startup state and unfinished goals
Autonomy: doing nothing - Nothing has happened that would justify doing anything.
```

Exactly one autonomy line for the whole session. The loop woke on the startup signal,
considered, and declined for lack of evidence; no further wakes occurred because nothing
signalled. Time passing did not manufacture a reason to act, which is the property the
whole design exists to guarantee.

Note that the hold is measured from process start rather than from runtime-ready, and
startup alone takes around twenty-five seconds, so a useful hold is sixty seconds or
more.

**Outstanding:** memory consolidation into the state packet, Phase 7 (neural emotion
model, training-data export), and Phase 8 (avatar). `AffectController` remains the emotion path that actually drives
conversation, and no `Stimulus` is constructed anywhere in `ReviaSession`.

## Response latency

Measured on a real turn rather than estimated. An ordinary C++ question cost 18.3
seconds, of which 14.8 seconds was an internet lookup:

```
turn_total=18296ms  internet_lookup=14840ms  llama_wait_first_token=411ms
                    llama_decode_after_first_token=2888ms
```

`InternetLookupPolicy::ShouldLookup` ended in `return question && lowered.size() >= 12`,
so **any input containing a question mark and at least twelve characters triggered a web
round trip**. Nearly every question the user asked paid for it, and the lookup was four
times more expensive than generating the answer.

A lookup now requires actual evidence that fresh external facts are wanted: an explicit
request, which is always honoured, or a time-sensitive marker. Technical and programming
wording is excluded before the time-sensitive check, because "which version of C++ has
std::format" would otherwise leave the machine on the word "version".

The same question now costs **3.0 seconds**, a 5.7x improvement, verified by re-running
it. Remaining turn cost is almost entirely model decode, which is the honest floor for
this hardware.

**Known and unfixed.**

- Replies perform personality instead of delivering substance. Across three separate
  technical questions Revia produced a characterful comment about the topic and never
  answered it ("A mutex (mutual exclusion) is basically a digital doorman." and nothing
  further). This is not downstream truncation: `ConversationStylePolicy::RefineReply`
  was tested directly and preserves a full answer both with empty context and against
  overlapping replayed history, and decode timings show the model itself stopping early.
  A profile-prompt rule requiring the answer alongside the personality was added and did
  **not** fix it. The likely cause is that the combined system prompt, state packet,
  style guidance, and humanization block push hard enough toward personality that
  substance loses, but that has not been isolated.
- `speech_service_stop` took 55.6 seconds on one shutdown, almost certainly a generation
  worker blocked in an uncancellable Qwen HTTP request. Not investigated.
- `memory_classification` costs about 7.5 seconds after a turn. It runs in the
  background and does not delay the reply, but it is large.
