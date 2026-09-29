# Revia (R.E.V.I.A) current state: capabilities, architecture, extension points, gaps

Scope: the local repository `/home/user/R.E.V.I.A`, branch `claude/hopeful-faraday-j2xb07`, HEAD `6c02631` (2026-09-28). All sources below are repo files; links are repo-relative paths (line numbers as read on 2026-09-28/29). Status vocabulary is the README's own: **Verified live** = seen working in real runtime logs on the dev PC; **Tested** = built and covered by automated tests, not confirmed live; **Not yet** = does not exist or has not run ([README.md L13](README.md)). Codebase size (measured with `wc -l`): ~111k lines of C++ in `Public/`, `Private/`, `Desktop/`; ~43k lines in `Tests/*.cpp`; 93 entries in `Tests/`; 385 files under `Public/`+`Private/`.

---

## (a) Learning and self-improvement

### Takeaway
Revia "learns" through evidence counters, reviewable memories, sourced read-only research, and a human-gated code-proposal loop (`/improve`) that builds and tests patches in a private copy. There is no weight fine-tuning, no self-applied change, and `/improve` is only "Tested" (scripted model), not verified live.

### Cited Findings
- Status table: "Reviewing her own code and proving her suggestions (`/improve`)" is **Tested**, "covered by automated tests with a scripted model" — [README.md L32](README.md)
- `/improve` triggers: (1) a measured weakness from self-assessment (e.g. "voice phrase generation repeatedly exceeds 12 seconds"), (2) idle review "every couple of hours of idle time" walking her own source, (3) user request `/improve review <file|area>` — [README.md L415-419](README.md)
- Proposal constraints: model (8B Expert if loadable, else Main) is shown ~220 lines, may propose at most one change; change must match exactly one place, stay under ~60 lines, touch only product code (`Private/`, `Public/`, `Desktop/`, Python voice worker), and must not add process launch, network, file deletion or registry access; benefit >= 0.6 (idle) or >= 0.4 (evidence-directed), risk <= 0.5 — [README.md L423-426](README.md); thresholds in `improvement` block — [Config/settings.json](Config/settings.json)
- Proof: patch applied to `%LOCALAPPDATA%\Revia\ImprovementWorkbench`, full build + all test suites; unchanged copy retested on failure; one compiler-error-guided repair; first proof ~9 min, later ~7 min on dev PC; builds run only after 10 min user absence at below-normal priority — [README.md L428-429](README.md)
- She "never edits her real source"; user applies via `git apply`; verdicts with reasons feed later reviews; reviews pause while 5 proven proposals await a decision — [README.md L73](README.md), [README.md L444-446](README.md)
- Honest limit stated by repo: "A local 4B or 8B model is a modest reviewer. Most reviews find nothing ... 'Proven' means it compiles and breaks no test, not that it makes her better" — [README.md L448](README.md)
- Self-assessment engine counts slow turns, Expert routes, internet failures, memory failures, voice stalls; default conclusion "Not enough evidence yet."; "Evidence collector only ... cannot change settings, install a model, edit source, or widen a capability" — [Public/Learning/selfAssessment.h](Public/Learning/selfAssessment.h)
- `LearningReview` draws `Planning` and `Initiative` lessons from goal outcomes and initiative proposal counters, minimum 4 samples; an approved lesson becomes an ordinary memory; "cannot change a capability, a budget, a policy, or any code" — [Public/Learning/learningReview.h](Public/Learning/learningReview.h)
- The only automatic policy update is initiative rate halving below a precision floor, computed inside `AttentionPolicy` — [Public/Learning/learningReview.h](Public/Learning/learningReview.h)
- Curiosity research: read-only, bounded, sourced; a finding is saved as memory only if it cites a supplied source URL — [docs/ARCHITECTURE.md L382-385](docs/ARCHITECTURE.md), [docs/ROADMAP.md L165-178](docs/ROADMAP.md)
- Known limit: "Self-directed learning is limited to evidence, read-only research, reviewable memory, and suggestions" — [README.md L544](README.md)
- Personality development: 16 traits as base + learned delta; evidence must agree across several observations (four-observation threshold mentioned) before a trait moves; changes bounded per step, lifetime-capped, reversible — [docs/ROADMAP.md L301-303](docs/ROADMAP.md), [docs/ROADMAP.md L403-409](docs/ROADMAP.md), [docs/ROADMAP.md L436-437](docs/ROADMAP.md)
- A learned (trained) computer-control ranker exists as an experiment: `Tools/Computer/` (`dataset.py`, `features.py`, `train.py`, `parity.py`) is a dependency-free CPU linear baseline; "What is qualified today: Nothing"; learned mode stays experimental — [docs/COMPUTER_CONTROL.md L364-378](docs/COMPUTER_CONTROL.md), [docs/COMPUTER_CONTROL.md L516-535](docs/COMPUTER_CONTROL.md)
- Roadmap open: "Benchmarking a performance proposal before and after, so 'faster' is measured rather than estimated" (⬜); goal/proposal lesson review is 🟡 — [docs/ROADMAP.md L173](docs/ROADMAP.md), [docs/ROADMAP.md L178](docs/ROADMAP.md)
- Phase 7 (neural emotion model, training-data export) is outstanding — [docs/ROADMAP.md L515-516](docs/ROADMAP.md)

### Inferences
- Compared with assistants that "learn" via fine-tuning or cloud memory, Revia's learning is deliberately conservative: counters + memories + human-approved patches. It is closer to a supervised self-review workflow than autonomous self-modification. (inferred)
- The `/improve` loop depends on the build toolchain being present on the user's PC (Qt/MinGW/CMake installed by setup), so it is a developer-install feature, not something a prebuilt end-user release could do (Phase 3 package "not started" per [docs/PORTABILITY.md](docs/PORTABILITY.md)). (inferred)

### Gaps
- No live evidence in the repo that `/improve` has produced an accepted proposal with a real (unscripted) model; README marks it Tested only.
- No LoRA/fine-tuning or preference-learning pipeline for the chat model was found.

---

## (b) Coding assistance

### Takeaway
Coding help is conversational: the router can send hard debugging/architecture questions to the Qwen3-VL 8B Expert, she can read the clipboard on request, and she "thinks" via a visible self-inquiry pass. There is no IDE/repo integration, no code execution for user code, and the repo itself records that technical answers are often personality-heavy and substance-light.

### Cited Findings
- Tier roles: Main (Qwen3.5 4B) handles "moderate coding"; Expert (Qwen3-VL 8B) handles "Difficult debugging, architecture, large technical context, and hard vision" — [docs/ROADMAP.md L75-80](docs/ROADMAP.md)
- Router "must judge cognitive difficulty, not message length" ("Why is this deadlocking?" is short but Expert) — [docs/ROADMAP.md L82](docs/ROADMAP.md), [Public/Intelligence/intelligenceRouter.h](Public/Intelligence/intelligenceRouter.h)
- Self-inquiry: on real questions/tasks she asks herself 2–4 questions, answers them, shows a "Revia is thinking" block; adds one bounded Main call (a few seconds); `conversation.selfInquiryScope` `"questions"` (default) or `"hard"` — [README.md L49](README.md), [Config/settings.json](Config/settings.json)
- Iterative investigation loop exists, but "No check executor is wired in this pass, so every round is reasoning only"; model output is never promoted to observed evidence — [Private/Runtime/conversationRuntime.cpp L671-677](Private/Runtime/conversationRuntime.cpp), [Public/Agents/investigationAgent.h](Public/Agents/investigationAgent.h)
- Clipboard: "Ask about 'the code I just copied' ... she reads it for that one question. It is not saved, and text that looks like a password or key is withheld" (commit `92d20e6`) — [README.md L70](README.md), [Public/Perception/clipboardText.h](Public/Perception/clipboardText.h)
- Files: default approved root only `%USERPROFILE%\Documents\ReviaSandbox`; reads up to 1 MiB without asking; writes need confirmation — [Config/capabilities.json](Config/capabilities.json), [README.md L220](README.md)
- "She never gets an unrestricted shell, and model text never becomes a shell command" — [README.md L78](README.md), invariant 1 in [docs/ARCHITECTURE.md L466](docs/ARCHITECTURE.md)
- Known and unfixed: "Replies perform personality instead of delivering substance. Across three separate technical questions Revia produced a characterful comment about the topic and never answered it ('A mutex ... is basically a digital doorman.' and nothing further)"; a profile-prompt rule did not fix it; cause not isolated — [docs/ROADMAP.md L546-555](docs/ROADMAP.md)
- Web lookups for technical wording were removed from automatic triggering after a C++ question cost 18.3 s (14.8 s lookup); same question then 3.0 s — [docs/ROADMAP.md L520-542](docs/ROADMAP.md)
- Context per tier is 8192 tokens (Main, Fast, Expert) — [Config/settings.json](Config/settings.json)
- Recent fix: exact token counting via llama-server `/tokenize`; before, 1 token/byte meant an 8K context held ~6.8 KB and a 20-exchange conversation kept 1 earlier message, now 36 (commit `8707ffd`) — git log; [Private/LLM/contextFitting.cpp](Private/LLM/contextFitting.cpp)

### Inferences
- With an 8K context and a ~5 KB system prompt (per commit `8707ffd` message), Revia cannot hold large code files or multi-file context; "large technical context" for Expert is bounded by the same 8K setting. (inferred)
- Compared with coding assistants (Copilot, Claude Code, Cursor), Revia lacks repo indexing, tool-using edit/run loops for user projects, and larger models; its coding role is "chat buddy that can read what you copied". (inferred)

### Gaps
- No benchmark of coding answer quality exists in the repo (the `/eval` corpus is conversational, see [docs/CONVERSATION_QUALITY.md](docs/CONVERSATION_QUALITY.md)).
- Whether the investigation loop runs by default (`iterativeEnabled`, `maximumRounds`) was not confirmed from settings.

---

## (c) Entertainment: singing, chat, starting conversations

### Takeaway
Chat with persistent personality/mood and evidence-driven initiative are **Verified live**; singing is karaoke playback of user-supplied WAVs (Tested), not generated singing; drawing (SVG), a working document, and SD-Turbo images (off by default) are Tested.

### Cited Findings
- Chat with streaming replies, TTFT ~0.4–0.5 s: **Verified live**; personality, persistent mood, likes/opinions, relationships: **Verified live**; curiosity, initiative, self-directed reflection: **Verified live** — [README.md L17-25](README.md)
- Karaoke: **Tested**, "You must supply the songs"; songs in `RuntimeData/Songs/<name>/` with `instrumental.wav`, `vocal.wav`, `song.json` (title, gains, timed lines); "her Qwen3-TTS voice cannot sing a melody"; to sound like her, convert the vocal with an external voice-conversion tool first — [README.md L31](README.md), [README.md L375-407](README.md)
- Singing has its own audio device/thread/owner (`SongLibrary`, `PerformanceRuntime`); presentation events include `StartedSinging`, `VocalStarted`, `VocalEnded` to drive a mouth during songs — [docs/ARCHITECTURE.md L265](docs/ARCHITECTURE.md), [Public/Presentation/presentationEvents.h](Public/Presentation/presentationEvents.h)
- `/draw` SVG diagrams and `/write`, `/revise`, `/scene`, `/undo` working document: **Tested** — [README.md L28](README.md), [README.md L333-335](README.md)
- `/imagine` (SD-Turbo, 512×512, 4 steps, ~4.2 GB VRAM): **Tested**, off by default, needs `Tools\InstallImageModel.ps1` — [README.md L34](README.md), [README.md L230-236](README.md)
- Initiative limits: at most 4 unprompted utterances/hour, >= 15 min apart (`cooldownSeconds` 900), dismissal backoff 3600 s, `minimumConfidence` 0.72, waits for typing pause (4 s), quiet in full-screen apps and when another app uses the mic (call) — [README.md L67](README.md), [Config/settings.json](Config/settings.json), commit `2c705f5`
- Conversation starters are event-driven cues: `FocusCompleted`, `ReturnedToApplication`, `ContextSwitching`, `SelfDirectedCuriosity`, `VisualIssue`; "elapsed time ... cannot wake the worker or create a cue" — [Public/Initiative/conversationStarter.h](Public/Initiative/conversationStarter.h), [docs/ARCHITECTURE.md L228-235](docs/ARCHITECTURE.md)
- Curiosity nominates `silence`, `speak`, `research`, plus idle `think`, `observe`, `create`, `computer`; autonomous activity owner allows one active execution, 2-min interval, 6 activities/rolling hour; private creations saved as notes under `RuntimeData/Workspace/Notes` — [docs/ARCHITECTURE.md L256](docs/ARCHITECTURE.md), [docs/ARCHITECTURE.md L343-365](docs/ARCHITECTURE.md)
- Drives: seven drives (boredom does not decay); "a timer alone can never produce an activity"; 70-second live hold produced exactly one "doing nothing" decision — [docs/ROADMAP.md L439-509](docs/ROADMAP.md)
- Emotion model: 20-component `EmotionVector`, rule-based appraisal (`RuleEmotionModel`), mood momentum; appraisal is the live emotion path — [docs/ROADMAP.md L294-402](docs/ROADMAP.md)
- Character: "bright young digital intelligence: intensely curious, playful ... occasionally bratty"; "not presented as a literal child ... never sexualized. Affection remains friendly and non-romantic" — [docs/REVIA_CHARACTER_DESIGN.md](docs/REVIA_CHARACTER_DESIGN.md)
- Conversation-quality contract with `/eval` corpus (e.g., no stock "What's on your mind?" tails, let anger show, no invented physical scenes); evaluation turns change no state — [docs/CONVERSATION_QUALITY.md](docs/CONVERSATION_QUALITY.md)
- Open roadmap items: evaluate initiative precision with accepted vs dismissed openings (⬜); cheap relevance scoring (⬜); F7/F8 live personality acceptance outstanding — [docs/ROADMAP.md L159-160](docs/ROADMAP.md), [docs/ROADMAP.md L515](docs/ROADMAP.md)

### Inferences
- Revia's "starting conversations" design is more restrained than typical AI VTubers (which often react constantly to chat); it optimizes for a desktop companion that stays silent by default. (inferred)
- Singing is a weak point versus AI VTubers that use singing voice synthesis / RVC-style conversion; Revia delegates that to external tools. (inferred)

### Gaps
- No games/minigames, trivia, or interactive storytelling modes were found beyond `/scene` in the working-document commands (not investigated in detail).
- Live precision of initiative (how often openings are welcomed) is not measured per the roadmap.

---

## (d) Playing games, computer control, internet access

### Takeaway
Computer control is a heavily gated, typed-action system (files, UI Automation, and optional pointer/keyboard) that is off by default for input and only "Tested"; it is designed for supervised desktop tasks, not real-time game play. Internet access is a query-only visible browser (Verified live, ~11 s per lookup) with no arbitrary URL fetching.

### Cited Findings
- Filesystem actions and Windows UI Automation: **Tested**, confined to `Documents\ReviaSandbox` and approved apps; driving the desktop (pointer, keyboard, app launch): **Tested**, **off by default**, exercised against a desktop test fixture — [README.md L29-30](README.md)
- Default approved apps `notepad.exe`, `explorer.exe` with named controls only; `desktopControl` pointer/keyboard/appLaunch/rawCoordinates/visualTargeting all `false`; `maxInputActionsPerMinute` 30, `minimumInputIntervalMs` 120, `maxTypedCharacters` 512; `maxDesktopActionsPerMinute` 12 — [Config/capabilities.json](Config/capabilities.json)
- Two scopes: `approved_applications` (input confined to verified foreground window of an allowlisted exe; Win-key/app-switch chords refused) and `whole_desktop` (pointer anywhere; still refuses terminals, script hosts, `regedit`, `win+r/x/s/i` unless `allowCommandSurfaces`) — [README.md L452-476](README.md), [docs/ARCHITECTURE.md L468-469](docs/ARCHITECTURE.md)
- Emergency stop: hold Ctrl+Alt+Shift, Stop button, or `/desktop stop`; does not go through model or turn queue — [README.md L465](README.md), `DesktopInputGuard` [docs/ARCHITECTURE.md L263](docs/ARCHITECTURE.md)
- Computer-control decision providers: modes `legacy` (default), `shadow`, `assisted` (deterministic `RoutineComputerPolicy` when confident), `learned` (requires qualified artifact; none qualified) — [docs/COMPUTER_CONTROL.md L44-56](docs/COMPUTER_CONTROL.md), [Config/settings.json](Config/settings.json)
- Natural-language operate: "open Microsoft Edge and pull up Facebook" routes to the typed action path deterministically (`DetectOperateRequest`), still policy/confirmation/rate-limit/audit gated — [Public/Planning/operateIntent.h](Public/Planning/operateIntent.h)
- Background tasks: `/goal` and "operate" run on their own thread; "cancel the task" or Stop ends them (commit `df2d14b`) — [README.md L68](README.md)
- Multi-step goals are 🟡 Partial: "Rehearsal, budgets, verification, confirmation, and resume exist; unattended delivery remains deliberately narrow"; unattended approved jobs ⬜ — [docs/ROADMAP.md L30](docs/ROADMAP.md), [docs/ROADMAP.md L206-216](docs/ROADMAP.md)
- Games: vision action parser notes that "A game, an Unreal or UMG surface, a canvas" lack a useful UIA tree and pointer targets are "described, not named" — [Private/Vision/visionActionParser.cpp L23](Private/Vision/visionActionParser.cpp), [Private/Vision/visionActionParser.cpp L149](Private/Vision/visionActionParser.cpp)
- Game-related hooks that exist: a `game` Presence adapter source (text-only conversation events), and a `SpeechOwner::Game` priority ("A reaction to something happening in a game") — [Config/settings.json](Config/settings.json) `presence.allowedAdapters`, [Public/Speech/speechCoordinator.h L45-50](Public/Speech/speechCoordinator.h)
- Initiative stays quiet over a full-screen game/video/presentation (`initiative.suppressWhenFullScreen`) — [README.md L67](README.md)
- Screen-context questions like "what game is this" are recognized as local screen questions — [Private/Runtime/conversationRuntime.cpp L95](Private/Runtime/conversationRuntime.cpp)
- Internet: web lookups in a visible browser **Verified live**, ~11 s per lookup, shows query and sources — [README.md L24](README.md)
- Internet design: model supplies a plain query only, never a URL/selector/script; visible mode uses a dedicated Edge/Chrome profile (Node `Tools/Browser/browserHost.mjs`) following public HTTPS search results; API mode fixed to `api.duckduckgo.com` / `en.wikipedia.org`; rejects private targets, non-GET/HEAD, downloads, dialogs — [docs/ARCHITECTURE.md L284-292](docs/ARCHITECTURE.md), [Config/capabilities.json](Config/capabilities.json)
- Limits: `maxRequestsPerMinute` 12, `maxResults` 5, `visibleBrowserMaxPages` 3; `/internet on|manual|off` — [Config/capabilities.json](Config/capabilities.json), [README.md L321](README.md)
- Remote PCs: ⬜ "Safety model is designed; runtime support is not built" — [docs/ROADMAP.md L34](docs/ROADMAP.md), [docs/ROADMAP.md L222-224](docs/ROADMAP.md)

### Inferences
- No game-playing agent exists (no game APIs, no input loop tuned for real-time play); the rate limits (30 inputs/min, 120 ms min interval) and per-step confirm/verify loop make real-time game play infeasible as designed. Games are currently something she *watches* via screen awareness, not plays. (inferred)
- Compared with Neuro-sama-style game integrations (game-specific APIs/mods), Revia's `game` adapter only carries text chat events; a game integration would need a new connector plus a new action family. (inferred)
- Internet access is narrower than typical agents (no arbitrary URL fetch, no logged-in browsing, no downloads). (inferred)

### Gaps
- No evidence of any live whole-desktop control session in logs; README only claims fixture testing.
- No list of which real applications the operate path has been demonstrated on beyond "three applications and eight task shapes" in the generalization matrix ([docs/COMPUTER_CONTROL.md L531-535](docs/COMPUTER_CONTROL.md)).

---

## (e) Memory and tracking who she is talking to

### Takeaway
Memory is solid and Verified live: SQLite structured memory with FTS5 + nomic embeddings (reciprocal-rank fusion), a bounded conversation archive, and (as of 2026-09-28) background LLM history compaction plus exact token counting. Speaker identity is textual/platform-ID based only (no voice biometrics); relationships are per-entity, evidence-driven, and persisted.

### Cited Findings
- Long-term memory + conversation history (SQLite + embeddings): **Verified live** — [README.md L19](README.md)
- Stores: `build/debug/Memory/revia_memory.db` (facts), `revia_conversations.db` (conversation); sensitive-looking turns (passwords, keys) not saved — [README.md L52](README.md)
- Hybrid retrieval: memories embedded with document prefix, queries with query prefix, BM25/FTS5 + cosine fused by reciprocal-rank fusion; embedding failures degrade to FTS — [docs/ARCHITECTURE.md L270](docs/ARCHITECTURE.md)
- Memory evaluation runs after the visible reply (background), `memory_classification` costs ~7.5 s in background — [docs/ARCHITECTURE.md L216](docs/ARCHITECTURE.md), [docs/ROADMAP.md L558-559](docs/ROADMAP.md)
- Embedding backfill: scans <= 25 rows missing vectors, retries with bounded backoff — [docs/ARCHITECTURE.md L103-123](docs/ARCHITECTURE.md)
- Archive limits: `maxSessions` 200, `maxTurnsPerSession` 500, `maxTurnCharacters` 8000, `restoreTurns` 6 — [Config/settings.json](Config/settings.json)
- History compaction (commit `6c02631`): when kept conversation reaches ~3/4 of budget, Main summarizes the oldest half in the background after a reply (next user message preempts it); newest exchanges stay verbatim; summary shown in Activity feed, saved with archive under the same secret filter, restored after restart, removed by `/history forget`; toggle `conversation.historyCompactionEnabled` — [README.md L53](README.md), [Public/Agents/historyCompactor.h](Public/Agents/historyCompactor.h), git log
- Exact token counting via llama-server `/tokenize` (commit `8707ffd`): 20-exchange conversation now keeps 36 earlier messages vs 1 in an 8K context — git log; [Private/LLM/LLamaCPP/llamaCppService.cpp L2319](Private/LLM/LLamaCPP/llamaCppService.cpp)
- `/history <words>` search and `/history forget` — [README.md L320](README.md)
- Person attribution: "This is textual attribution, not biometric recognition or authentication, and it does not grant capabilities"; first named person adopts anonymous local history; fresh session starts anonymous and "does not guess who is at the keyboard" — [docs/ARCHITECTURE.md L60-73](docs/ARCHITECTURE.md)
- Entity IDs namespaced (`local:user`, `adapter:discord:name`); adapter author strings sanitized; stream events require `author_id` ("display names are not identities") — [docs/ROADMAP.md L361-364](docs/ROADMAP.md), [Tools/Presence/README.md](Tools/Presence/README.md)
- Relationship evidence is deterministic: "A model never assigns relationship numbers ... claiming to be trusted produces no trust"; warmth capped by familiarity (`0.25 + 0.75 * familiarity`); ~400 exchanges to "you know them well, you like them, you trust them" — [docs/ROADMAP.md L365-386](docs/ROADMAP.md)
- Relationship dimensions shown in the read-only Mind tab: familiarity, affinity, trust, respect, irritation, resentment, exchange count — [docs/ROADMAP.md L426-431](docs/ROADMAP.md)
- Identity persisted atomically (schema version 2 file), saved every 30 s and on shutdown — [docs/ARCHITECTURE.md L39-58](docs/ARCHITECTURE.md)
- Quoted/reported speech attribution (`User`, `OtherPerson`, `Revia`) from explicit reporting phrases; "does not guess identities" — [Public/Core/speechAttribution.h](Public/Core/speechAttribution.h)
- Hands-free addressee gate: speech counts only if it names her (wake words "revia", "rivia", "revya", "reviya", "revea") or follows up within 20 s; during a call only her name counts — [Public/Speech/addresseeGate.h](Public/Speech/addresseeGate.h), [Config/settings.json](Config/settings.json)
- Open memory issues: ISSUE-0079 paraphrased memories still accumulate (embedding similarity of paraphrases 0.801–0.983 overlaps different claims 0.733–0.944); ISSUE-0080 contradictions kept but not superseded (no `superseded_by`); ISSUE-0087 two provenance classes unexercised live; ISSUE-0088 "would rather" misread as change marker — [docs/AI_PIPELINE_AUDIT.md L387-489](docs/AI_PIPELINE_AUDIT.md)
- Not yet: state packet's `memories` field never populated (memory inserted by `promptBuilder::BuildMessages` instead); autobiographical metadata (`emotionAtEncoding`, `relationshipAtEncoding`) and memory strength/decay outstanding — [docs/ROADMAP.md L419-425](docs/ROADMAP.md)
- Roadmap ⬜: "Represent strong memory, familiarity, partial recall, uncertainty, and forgotten detail naturally"; "Track preference evidence and confidence so opinions can evolve" — [docs/ROADMAP.md L146-147](docs/ROADMAP.md)
- Conflict: ROADMAP says "Evicted conversation is compacted into a deterministic bounded history summary" (✅) — [docs/ROADMAP.md L145](docs/ROADMAP.md); the 2026-09-28 commit `6c02631` replaced truncation to "first 280 characters" with a Main-model summary, so the roadmap wording predates the new LLM compaction.

### Inferences
- Revia's relationship model (numeric, evidence-gated, per-entity, persistent) is more structured than most AI companions', but identity resolution relies on the user stating their name or on platform IDs; a second person at the same PC is indistinguishable unless they introduce themselves. (inferred)
- Hands-free STT has no diarization/speaker verification; the Discord connector gets per-user audio streams from Discord, which provides speaker identity there. (inferred from [Tools/Presence/DiscordVoice/README.md](Tools/Presence/DiscordVoice/README.md) "decodes each human speaker's audio")

### Gaps
- No voice-print/speaker-recognition or face recognition component was found.
- Memory quality at scale (thousands of memories) is not measured in the repo.

---

## (f) VTuber / streaming: avatar bridge, presence, Discord, web demo, Twitch/YouTube

### Takeaway
Only the contracts exist: an avatar state/event file bridge (no renderer, `target: "unselected"`), a Presence file inbox/outbox for Discord/stream/game text (Tested, off by default), a Discord voice connector (18/18 tests, never joined a live call), and a web text demo (built/tested, not deployed). There is no OBS integration and no Twitch/YouTube connector.

### Cited Findings
- "Animated avatar (Live2D/VRM), OBS, streaming platforms: **Not yet** — Only the state/event contract exists; there is no renderer" — [README.md L38](README.md)
- Avatar bridge v1: `RuntimeData/Presence/avatar_state.json` atomic snapshot with `phase` (offline/idle/listening/thinking/responding/speaking/acting/waiting/blocked/error), `expression`, `affect_intensity`, `speaking`, `mouth`, `listening`, `attention`, `gaze_target`, `conversation_momentum`, `sequence`, `timestamp`; plus ordered `avatar_events.jsonl` (rotates at 4 MiB) — [Tools/Presence/AVATAR_BRIDGE.md](Tools/Presence/AVATAR_BRIDGE.md), [Config/settings.json](Config/settings.json)
- `Config/avatar.json`: character palette/expression map (neutral, curious, happy, excited, playful, bored, sulky, sad, angry, frustrated, concerned, confused, focused); `renderer.target: "unselected"`, `lipSync: "audio_amplitude"`, `visemes: "future"` — [Config/avatar.json](Config/avatar.json)
- Roadmap: avatar 🟡 "a real Live2D/VRM renderer and model are not selected or live-verified"; "Locomotion, physics, IK, shaders, and desktop walking remain later" — [docs/ROADMAP.md L35](docs/ROADMAP.md), [docs/ROADMAP.md L230-232](docs/ROADMAP.md)
- `PresentationBus` with `IPresentationSink` ("A debug console now; a Live2D model, a VRM rig, or an OBS overlay later"); wired sinks are the avatar sink and `DebugPresentationSink` — [Public/Presentation/presentationBus.h](Public/Presentation/presentationBus.h), [Private/Runtime/reviaSession.cpp L846-848](Private/Runtime/reviaSession.cpp)
- Presentation events: EmotionChanged, MoodChanged, Started/StoppedListening, Thinking, Speaking, SpeechInterrupted, Researching, ComputerTask, Singing, VocalStarted/Ended, CuriosityRaised, BecameIdle, UserInterrupted, GoalCompleted/Failed; private reasoning is never a presentation event — [Public/Presentation/presentationEvents.h](Public/Presentation/presentationEvents.h)
- Presence adapters: JSON file per event into `RuntimeData/Presence/Inbox`; reply to `Outbox`; allowed sources `discord`, `stream`, `game`; text starting with `/` rejected; public turns get channel-scoped history only (no private memory, desktop/camera, automatic lookup) — [Tools/Presence/README.md](Tools/Presence/README.md)
- Presence defaults: `externalAdaptersEnabled` false, `maxAdapterEventsPerMinute` 30, `publicContextTurns` 6, `maxPublicConversationContexts` 32, `streamReplyCooldownSeconds` 4, `requireAddressedStreamMessages` true, `speakStreamReplies` false — [Config/settings.json](Config/settings.json)
- Discord text chat through Presence inbox: **Tested**; Discord voice: **Tested**, "Connector tests pass 18/18; no live Discord join has been done yet" — [README.md L36-37](README.md)
- Discord voice details: Node connector (`@discordjs/voice` 0.19.2 with DAVE E2EE, `opusscript`), up to four speakers at once, phrase ends after 0.9 s silence (20 s max), she does not listen while talking, cannot be interrupted in Discord, needs Qwen voice (no SAPI fallback), `replyMode` `addressed` or `conversation` — [README.md L238-274](README.md), [Tools/Presence/DiscordVoice/README.md](Tools/Presence/DiscordVoice/README.md)
- Web demo: `WebGuestRuntime` with a separate `messageRouter`, fixed `PublicGuestProfile`, no memory/desktop/internet/speech; relay (Node, `ws` 8.21.3) deployable to Render; native listener `127.0.0.1:17864` routes `/web/v1/status|turn|cancel|end|control`; off unless `REVIA_WEB_ENABLED=1` — [docs/WEB_DEMO.md](docs/WEB_DEMO.md), [Tools/Presence/WebDemo/README.md](Tools/Presence/WebDemo/README.md)
- Web demo status: local tests pass (ctest 10/10, npm 41 passed, native integration 1 passed); Portfolio static page published with `enabled: false`, "there is no deployed live chat relay"; no Render service created — [docs/WEB_DEMO_EVIDENCE.md L5](docs/WEB_DEMO_EVIDENCE.md), [docs/WEB_DEMO_EVIDENCE.md L65](docs/WEB_DEMO_EVIDENCE.md)
- Web demo has "no microphone or voice controls"; voice is future work — [docs/WEB_DEMO.md](docs/WEB_DEMO.md)
- Twitch/YouTube: no connector implementation found (grep for "twitch|youtube" in `.cpp/.h/.mjs` matched only tests/docs/comments); `stream` source contract with `author_id`, roles `viewer`, `broadcaster`, `moderator` is the intended integration point — [Tools/Presence/README.md](Tools/Presence/README.md)
- Known limit: "No avatar renderer, OBS, or streaming-platform connector exists yet" — [README.md L546](README.md)

### Inferences
- Revia is architecturally VTuber-ready (clean state contract, public/private isolation, multi-platform text adapter shape) but has zero shipped streaming surface; an AI VTuber comparison would put her behind open-source stacks that already ship VTube Studio/Live2D/OBS/Twitch integration. (inferred)
- The file-based inbox/outbox IPC (150 ms poll) is simple and safe but adds latency and is not a streaming protocol; a real-time avatar would read the avatar snapshot file. (inferred)

### Gaps
- No evidence any Live2D/VRM renderer, VTube Studio plugin, or OBS WebSocket client has been prototyped.
- No lip-sync amplitude/viseme stream is emitted for TTS audio beyond the `mouth`/`speaking` gate (renderer-side smoothing expected).

---

## (g) Voice: TTS, STT, barge-in, hands-free

### Takeaway
Local voice is Verified live: Qwen3-TTS 0.6B voice-cloned speech pipelined sentence-by-sentence across two GPUs (SAPI fallback) and whisper.cpp Distil-small.en STT with push-to-talk and hands-free wake-word gating. Barge-in is only Tested; TTS is not true streaming; STT is English-only.

### Cited Findings
- Qwen3-TTS cloned voice across two GPUs: **Verified live**; SAPI fallback: **Verified live**; STT push-to-talk and hands-free: **Verified live** (~0.3–1.0 s/utterance); barge-in: **Tested**, "not confirmed in recent logs"; Voice Studio profiles: **Verified live** — [README.md L21-27](README.md), [README.md L35](README.md)
- Models: reply voice `Qwen3-TTS-12Hz-0.6B-Base` (RTX 5070 BF16 + RTX 2070 Super FP32); voice creation `Qwen3-TTS-12Hz-1.7B-VoiceDesign` loaded on demand; STT `ggml-distil-small.en.bin` (fallback `ggml-small.en.bin`) on RTX 2070 Super — [README.md L352-354](README.md)
- Voice speed (10 scripted live sessions, 23 Sep 2026): per-sentence RTF ~0.35–0.39 (5070) and ~0.56–0.72 (2070S); first sentence audible 1.2–2.0 s after reply start when a card is free; batching disabled because it caused 20–30 s silences; cached Reflex phrase plays < 0.1 s — [README.md L362-371](README.md)
- "The installed `qwen-tts` 0.1.1 API does not expose true incremental generated PCM ... Revia therefore implements text-pipelined TTS with complete sentence audio" — [docs/TTS_PERFORMANCE.md L11](docs/TTS_PERFORMANCE.md)
- Conflict: ROADMAP says "first phrase generation remains roughly 8–17 seconds in measured runs" and RTF above 1 — [docs/ROADMAP.md L65-66](docs/ROADMAP.md); README's newer 23 Sep 2026 measurements report 1.2–2.0 s first sentence and RTF < 1 per sentence — [README.md L364-366](README.md). TTS_PERFORMANCE passes 2–4 describe CUDA-graph work; `qwenCudaGraph` and `qwenTalkerGraph` are `true` in current settings — [Config/settings.json](Config/settings.json)
- TTS doc notes "Nothing here has been heard by a human" for batched pass structural checks — [docs/TTS_PERFORMANCE.md L452-464](docs/TTS_PERFORMANCE.md)
- Qwen3-TTS runs as authenticated loopback Python/PyTorch workers (`Tools/qwen_tts_service.py`), C++ owns lifecycle/scheduling; `OrderedSpeechQueue` releases sentences strictly in order — [docs/ARCHITECTURE.md L220](docs/ARCHITECTURE.md)
- Hands-free: VAD (`vadEnergyThreshold` 900, `vadSilenceMs` 350), `requireWakeWord` true, `followUpSeconds` 20; detected speech no longer cancels work (commit `fb2b535`); another app using the mic (call) suppresses unprompted speech and requires her name (commit `2c705f5`) — [Config/settings.json](Config/settings.json), [Public/Perception/microphoneUse.h](Public/Perception/microphoneUse.h)
- STT language: `speechRecognition.language` `"en"` and both shipped models are `.en` English-only — [Config/settings.json](Config/settings.json), [Config/model_manifest.json](Config/model_manifest.json)
- Barge-in settings: `energyThreshold` 1400, `echoMarginMultiplier` 2.6, `consecutiveFramesRequired` 8 — [Config/settings.json](Config/settings.json)
- Unverified: "long repeated barge-in stress remains unverified" — [docs/ROADMAP.md L255](docs/ROADMAP.md)
- Known unfixed: `speech_service_stop` took 55.6 s on one shutdown (likely a blocked Qwen HTTP request) — [docs/ROADMAP.md L556-557](docs/ROADMAP.md)
- `flash-attn` has no verified Windows/Blackwell wheel, not recommended — [docs/ROADMAP.md L67](docs/ROADMAP.md)

### Inferences
- Versus cloud voice stacks (ElevenLabs/OpenAI realtime), Revia's voice is fully local but sentence-granular and GPU-hungry (Qwen needs ~4.6 GB free VRAM per `qwenMinimumFreeVramMiB`); there is no speech-to-speech/full-duplex model. (inferred)
- English-only STT limits non-English users and multilingual streams. (inferred)

### Gaps
- No emotion/prosody control in TTS tied to the emotion vector was confirmed (not investigated in `speechService`).
- No measurement of end-to-end speech-in to speech-out latency in hands-free mode was found.

---

## (h) Vision, screen awareness, camera

### Takeaway
Continuous multi-monitor screen awareness is Verified live (local VLM summaries every ~30 s or on window change, ~1.0–1.6 s each) and was just fixed so she no longer claims she cannot see; camera is a single still frame, off by default and only Tested.

### Cited Findings
- "Continuous screen awareness across all monitors: **Verified live** — Local vision analysis every ~30 s or on window change, ~1.0–1.6 s each" — [README.md L20](README.md)
- Settings: `continuousAwareness` true, `awarenessDebounceMs` 1500, `awarenessMinimumIntervalMs` 6000, `awarenessRefreshSeconds` 30, `awarenessMaxResponseTokens` 160 — [Config/settings.json](Config/settings.json)
- Main vision: Qwen3.5 4B + `mmproj-F16.gguf`; Expert vision: Qwen3-VL 8B + Q8 projector for "hard vision" — [README.md L349-350](README.md)
- Screenshots deleted immediately; only short summary kept; on-screen text treated as information, never instructions; `/perception pause|resume|forget` — [README.md L58](README.md), [README.md L228](README.md)
- Exclusions (`perception.excludedApplications`) hide activity metadata only and "do not mask pixels in screenshots sent to vision" — [docs/ARCHITECTURE.md L125-136](docs/ARCHITECTURE.md), [README.md L485](README.md)
- Recent fix (commit `6cd717c`): turn instructions were built before the screen look and said no observation was taken, so she claimed she could not see; now the look happens first; "look at my screen"/"my second monitor" now trigger a fresh look — git log
- Vision-to-action: one-shot foreground-window capture → Qwen3-VL bounded target region → `VisionUiaResolver` → exact UIA identity → policy/confirmation; never clicks a stale coordinate — [docs/ARCHITECTURE.md L210-214](docs/ARCHITECTURE.md), [docs/ARCHITECTURE.md L258-262](docs/ARCHITECTURE.md)
- Camera: one still frame, **Tested**, off by default; camera light only on while frame taken — [README.md L33](README.md), [README.md L62](README.md); `CameraSelection` uses durable symbolic link identity — [Public/Vision/cameraCaptureService.h](Public/Vision/cameraCaptureService.h)
- Roadmap: continuous camera watching "is not part of the current plan"; remote PCs and camera ⬜ — [docs/ROADMAP.md L226-228](docs/ROADMAP.md), [docs/ROADMAP.md L34](docs/ROADMAP.md)
- 🟡 change detection so visually identical states avoid model wakes — [docs/ROADMAP.md L158](docs/ROADMAP.md)
- Known limit: "Screen awareness summarizes what is visible. It is not a recording, not an OCR archive, and not permission to act" — [README.md L543](README.md)

### Inferences
- Ambient 30 s screen summaries are well suited to "commentating" a stream or game at low frequency, but not frame-rate game perception. (inferred)

### Gaps
- No OCR or video-understanding component beyond the VLM summary was found.

---

## Architecture: model tiers, backend abstraction, buses, skills, policy, planner, tasks, reminders

### Takeaway
Revia is a single C++20/Qt 6 process (`ReviaSession` as sole lifecycle owner) that supervises separate loopback worker processes (llama.cpp x3 + embeddings, whisper.cpp, Qwen3-TTS Python, visible browser Node, optional SD-Turbo). The LLM layer is hard-wired to llama.cpp: `llmBackendType` lists Ollama/OpenAI/LMStudio/CustomHttp as "Planned backends, not yet implemented", and `messageRouter` holds three concrete `llmService` members rather than a provider interface.

### Cited Findings
- Tiers: Reflex (deterministic C++, "Live verified at 18 ms"), Fast (`Qwen3.5-0.8B-Q4_K_M.gguf`, CPU, port 8082, fallback when Main has no GPU/down), Main (`Qwen3.5-4B-Q4_K_M.gguf` + `mmproj-F16.gguf`, port 8080), Expert (`Qwen3-VL-8B-Instruct-Unredacted-MAX.Q4_K_M.gguf` + Q8 mmproj, port 8083, `onDemand` true, `idleGraceSeconds` 300); embeddings `nomic-embed-text-v1.5.Q4_K_M.gguf` on CPU (port 8081); whisper-server port 8094; Qwen TTS 8092; image 8093; visible browser 8095 — [README.md L341-358](README.md), [Config/settings.json](Config/settings.json), [docs/ROADMAP.md L248](docs/ROADMAP.md)
- "only one model answers each turn"; routing decided before generation; `IntelligenceRouter` records requested/selected tier, model, reason, confidence, fallback — [README.md L48](README.md), [docs/ROADMAP.md L84-90](docs/ROADMAP.md)
- One identity packet (`RenderStatePacket`, `Public/Identity/reviaStatePacket.h`) is handed to every tier — [docs/ROADMAP.md L341-347](docs/ROADMAP.md)
- Backend enum: `None, Placeholder, LLamaCpp, // Planned backends, not yet implemented: Ollama, OpenAI, LMStudio, CustomHttp` — [Public/Library/enumLibrary.h L21-30](Public/Library/enumLibrary.h)
- `llmService` contains a concrete `llamaCppService llamaCpp;` and a `backendType` switch; methods include GenerateResponse, GenerateActionProposal, GenerateCuriosityPlan, Deliberate, SummarizeConversation, GenerateGoalPlan, GenerateCodeReview, GenerateComputerSubgoal, AnalyzeImage, EvaluateMemory, EmbedMemory — [Public/LLM/llmService.h](Public/LLM/llmService.h), [Private/LLM/llmService.cpp L14-105](Private/LLM/llmService.cpp)
- `messageRouter` members: `llmService llm; llmService fastLlm; llmService expertLlm;` plus `ModelResidencyManager` and optional `ModelLifetimeCoordinator` — [Public/Core/messageRouter.h](Public/Core/messageRouter.h)
- HTTP endpoints used: OpenAI-compatible `/v1/chat/completions`, `/v1/models`, `/v1/embeddings`, plus llama.cpp-specific `/props`, `/tokenize`, `/health` — [Private/LLM/LLamaCPP/llamaCppService.cpp](Private/LLM/LLamaCPP/llamaCppService.cpp), [Private/LLM/LLamaCPP/llamaCppEmbeddingService.cpp](Private/LLM/LLamaCPP/llamaCppEmbeddingService.cpp)
- Structured outputs are constrained by JSON schema (curiosity nomination, ambient vision, history compaction) — [docs/ARCHITECTURE.md L317-326](docs/ARCHITECTURE.md), [Public/Agents/historyCompactor.h](Public/Agents/historyCompactor.h)
- `llm.backend: "LLamaCpp"`, `autoStartServer: true`, `serverExecutable: ThirdParty/llama.cpp/llama-server.exe` — [Config/settings.json](Config/settings.json)
- "Nothing is sent to a cloud model"; workers are separate local processes on 127.0.0.1 so each GPU has its own CUDA context and a crash cannot take Revia down — [README.md L358](README.md)
- `InferenceScheduler` admits no more requests than llama.cpp slots and always prefers interactive over background work — [docs/ARCHITECTURE.md L216](docs/ARCHITECTURE.md)
- Turn path: Qt/CLI → `ReviaSession` → `InputArbiter` → `ConversationRuntime` → `TurnCoordinator` → `ConversationAgent` (+ style/response filters) → scheduler → llama.cpp; then `MemoryAgent` in background — [docs/ARCHITECTURE.md L189-214](docs/ARCHITECTURE.md)
- Event buses: `RuntimeEventBus`/`RuntimeEvent` (published from everywhere) and `PresentationBus` which narrows runtime events via `TranslateRuntimeEvent` — [Public/Presentation/presentationBus.h](Public/Presentation/presentationBus.h), [Public/Runtime/runtimeEvents.h](Public/Runtime/runtimeEvents.h)
- Policy pipeline: one typed `ActionRequest` → `CapabilityPolicy` → blocked / human confirmation / allowed → durable audit intent (JSONL, flushed before executor) → `ActionDispatcher` → Filesystem/UIA/internet executor → audit result — [docs/ARCHITECTURE.md L3-37](docs/ARCHITECTURE.md)
- Execution modes: `disabled`, `supervised` (default; auto-approve through `autoApproveRiskThrough` = `read_only`), `approved_scope`; capability `mode` can also be `owner_full_access` (stops confirmations only for reversible in-scope work) — [docs/ARCHITECTURE.md L272-282](docs/ARCHITECTURE.md), [README.md L478](README.md), [Config/capabilities.json](Config/capabilities.json)
- Policy headers: `actionApproval.h`, `capabilityEditor.h`, `capabilityPolicy.h`, `desktopActionRateLimiter.h`, `desktopAuthorization.h`, `desktopInputGuard.h`, `permissionStore.h` — [Public/Policy/](Public/Policy)
- Approvals asked without blocking UI; shutdown refuses pending ones (`QuestionRelay`, commit `3e50b4f`) — [Public/Core/questionRelay.h](Public/Core/questionRelay.h)
- Goal planner: `GoalRunner` is a "Bounded plan / act / observe / verify loop over the existing typed actions" with `NextStep` (step / finished / stuck / needsInput); goals rehearsed in disposable fixtures (`goalSandbox.h`) — [Public/Goals/goalRunner.h](Public/Goals/goalRunner.h), [docs/ROADMAP.md L212](docs/ROADMAP.md)
- Reminders: deterministic parser ("remind me in 10 minutes to...", "at 3pm", "set a timer"), max 100 pending, persisted, missed ones delivered late at next start, spoken when due (text only during calls) — [Public/Planning/reminders.h](Public/Planning/reminders.h), commit `7619979`
- Identity/emotion subsystems: `EmotionRuntime` (20-dim vector, mood), `RelationshipRegistry`, `DevelopmentEngine` (16 traits), `DriveState` (7 drives), `ActivityScheduler` — [docs/ROADMAP.md L286-518](docs/ROADMAP.md)
- Output cleaning: `ResponseFilter` removes leaked prompt text, control tokens, fake "User:" lines, loops, cut-off endings; optional AI review off (`responseFilter.aiReviewEnabled` false) — [README.md L54](README.md), [Config/settings.json](Config/settings.json)
- Module ownership table (Planning, Policy, Actions, Filesystem, Windows, Internet, Speech, Performance, Presence, Vision, Computer, Audit, Runtime, Emotion, Autonomy, Identity, Resources, Desktop, Agents, Memory, Visual, Content, Evaluation, LLM, Core) — [docs/ARCHITECTURE.md L138-166](docs/ARCHITECTURE.md)
- Build targets: `ReviaFoundation` static lib, `R_E_V_I_A` CLI, `ReviaDesktop` Qt app, `ReviaTests`, web-guest test executables — [CMakeLists.txt L125-415](CMakeLists.txt)

### Inferences
- Because the chat client already speaks OpenAI-style `/v1/chat/completions`, pointing `llm.host/port` at another OpenAI-compatible local server (Ollama, vLLM, LM Studio) with `autoStartServer: false` might partially work, but `/props`, `/tokenize`, and llama.cpp JSON-schema/grammar behavior are llama.cpp-specific and the code path is untested for that. A clean provider interface would be needed for cloud APIs (auth headers, remote hosts; the web demo explicitly requires a loopback model endpoint per [docs/WEB_DEMO.md](docs/WEB_DEMO.md)). (inferred, not verified)
- Tiers are fixed at three LLM slots (Fast/Main/Expert) as struct members; adding a fourth tier or a cloud "Expert" requires code changes in `messageRouter`. (inferred)

### Gaps
- Did not read `IntelligenceRouter` implementation to list exact routing heuristics.
- Did not confirm whether any local HTTP API exposes Revia's chat to arbitrary local clients beyond the Presence file IPC and the web-guest listener.

---

## Extension points and modularity: skills, tools, backends, output channels, plugins, MCP

### Takeaway
Extension is by compiled-in C++ components following a strict "typed request + policy + narrow executor + audit + tests" shape; the only out-of-process extension surfaces are the Presence JSON inbox/outbox, the avatar state files, and the web-guest loopback HTTP API. A `IReviaSkill`/`SkillManager` interface exists with one example skill, but no skill appears to be registered at runtime; there is no dynamic plugin loading and no MCP support.

### Cited Findings
- `IReviaSkill` interface: `Id`, `Capabilities`, `Start`, `Stop`, `HandleEvent`, `AvailableActions` (proposals), `Observations`; a skill "can be told what happened, it can say what it noticed, and it can propose an action. It cannot execute one"; "A skill has no identity, no mood, no memory and no voice of its own" — [Public/Skills/reviaSkill.h](Public/Skills/reviaSkill.h)
- `SkillManager` evaluates proposals against the shared `CapabilityPolicy`, forwards survivors to a sink owned by `ReviaSession`, sanitizes self-asserted authority (`SanitizeProposedRequest`), and converts observations into autonomy evidence — [Public/Skills/skillManager.h](Public/Skills/skillManager.h)
- Worked example `WorkspaceStatusSkill` watches one folder and proposes only `ReadFile` — [Public/Skills/workspaceStatusSkill.h](Public/Skills/workspaceStatusSkill.h)
- Runtime wiring: `ReviaSession` owns `skills::SkillManager skillManager;` and calls only `ContributeEvidence` (grep found no `skillManager.Add(...)` in `Private/` or `Desktop/`) — [Public/Runtime/reviaSession.h L819](Public/Runtime/reviaSession.h), [Private/Runtime/reviaSession.cpp L6277](Private/Runtime/reviaSession.cpp)
- Architecture rule for new abilities: "a typed request, capability-specific policy fields, a narrow executor, limits/timeouts, audit fields, and tests proving both the allowed path and denial path" — [docs/ARCHITECTURE.md L187](docs/ARCHITECTURE.md)
- "When a feature needs model logic, persistence, OS authority, and presentation, those are four components connected through typed values or events" — [docs/ARCHITECTURE.md L268](docs/ARCHITECTURE.md)
- Invariant 9: "New capabilities start disabled or supervised and earn unattended access through tests and explicit configuration" — [docs/ARCHITECTURE.md L475](docs/ARCHITECTURE.md)
- Output-channel seams: `IPresentationSink` (Live2D/VRM/OBS later) — [Public/Presentation/presentationBus.h](Public/Presentation/presentationBus.h); `SpeechOwner` priorities `System, Conversation, Research, Performance, Skill, Game, Autonomy` — [Public/Speech/speechCoordinator.h L33-51](Public/Speech/speechCoordinator.h); Presence inbox/outbox contract ("Keep each connector single-purpose") — [Tools/Presence/README.md](Tools/Presence/README.md)
- Action executor seam: `IActionExecutor.h`, `actionDispatcher.h` in `Public/Actions/` — [Public/Actions/](Public/Actions)
- Investigation loop has an empty `CheckExecutor` seam "for real checks" — [Private/Runtime/conversationRuntime.cpp L671-677](Private/Runtime/conversationRuntime.cpp)
- Backend seam: `llmBackendType` planned values (Ollama, OpenAI, LMStudio, CustomHttp) not implemented — [Public/Library/enumLibrary.h L21-30](Public/Library/enumLibrary.h)
- Class scaffolding helper `Tools/CreateReviaClass.py` exists — [Tools/](Tools)
- MCP: a case-insensitive grep for "mcp" / "model context protocol" across code, docs, configs found no matches in Revia code (only unrelated secret-detector strings for OpenAI key prefixes) — [Private/Memory/sensitiveContent.cpp](Private/Memory/sensitiveContent.cpp), [docs/AI_PIPELINE_AUDIT.md L43-44](docs/AI_PIPELINE_AUDIT.md)
- Roadmap rule: "Keep avatar work behind the replaceable Presence boundary; rendering never owns Revia's mind or authority" — [docs/ROADMAP.md L282](docs/ROADMAP.md)

### Inferences
- The skill interface is well-shaped for integrations (Discord/OBS/game/music) but is effectively a scaffold: no production skill is registered and skills must be compiled in. (inferred from grep)
- An MCP client could map naturally onto `IReviaSkill` proposals (tools → `SkillProposal`/`ActionRequest` through `CapabilityPolicy`), but that would require new typed action kinds since today's `ActionRequest` covers filesystem/UIA/desktop/web search. (inferred)
- New platform connectors (Twitch, YouTube, game mods) can be added today without touching C++ by writing a Node/Python process that speaks the Presence JSON contract, at the cost of file-poll latency and text-only I/O. (inferred)

### Gaps
- Did not verify whether `ActionRequest` kinds are an open enum or closed switch (affects how hard a new tool is).
- No documented "how to add a skill" guide was found beyond header comments.

---

## Known gaps and roadmap items stated by the repo

### Takeaway
The repo is explicit about what is unverified: fresh-PC setup, single-GPU/CPU runs, leak/stress tests, Discord live join, avatar renderer/OBS/platforms, prebuilt release, memory supersession, and a persistent "personality over substance" answer-quality bug.

### Cited Findings
- README "Not yet": animated avatar (Live2D/VRM), OBS, streaming platforms; clean install on a second PC, laptop/CPU-only machines — [README.md L38-39](README.md)
- README Known limits: Qwen voice not always faster than real time; screen awareness is not recording/OCR; self-directed learning limited; Discord voice not live and not interruptible; no avatar/OBS/platform connector; clean install/laptops/CPU-only/long stress not verified — [README.md L540-547](README.md)
- ROADMAP status: multi-step goals 🟡; hardware adaptation 🟡; one-command clean setup 🧪; remote PCs and camera ⬜; animated avatar 🟡 — [docs/ROADMAP.md L30-35](docs/ROADMAP.md)
- ROADMAP ⬜ items: Expert on 5070 alone vs two-GPU split benchmark; KV-cache/context/Flash Attention/quantization/offload benchmarks; natural memory strength/partial recall; preference evidence confidence; cheap relevance scoring (Fast); initiative precision evaluation; before/after performance benchmarking of proposals; prebuilt end-user release; unattended approved jobs — [docs/ROADMAP.md L125-216](docs/ROADMAP.md)
- Verification gates 🧪: physical one-GPU laptop run, CPU-only run, extended VRAM/RAM/handle leak test, long barge-in stress, fresh clone setup, missing/corrupt model and occupied-port matrix — [docs/ROADMAP.md L252-257](docs/ROADMAP.md)
- Suggested next slice: fresh-PC setup twice; one-GPU laptop + CPU/SAPI session; stop/barge-in leak test; re-evaluate FlashAttention when a wheel exists; keep benchmark history — [docs/ROADMAP.md L261-269](docs/ROADMAP.md)
- "Known and unfixed": personality over substance on technical questions; 55.6 s `speech_service_stop`; ~7.5 s background memory classification — [docs/ROADMAP.md L544-559](docs/ROADMAP.md)
- Outstanding persistent-mind work: F7/F8 live personality acceptance, memory consolidation into state packet, Phase 7 neural emotion model, Phase 8 avatar — [docs/ROADMAP.md L515-516](docs/ROADMAP.md)
- Phase 6 note: "Only `ContinueGoal` executes. Every other decided activity is recorded and published but explicitly reported as not yet carried out" — [docs/ROADMAP.md L480-482](docs/ROADMAP.md) (note: [docs/ARCHITECTURE.md L343-365](docs/ARCHITECTURE.md) describes think/observe/create/computer activities now executing via the shared activity owner, so this ROADMAP line may be stale)
- AI pipeline audit: "Revia's AI pipeline is not complete. This pass hardened eight specific paths and left seven findings open"; open ISSUE-0079, 0080, 0087, 0088 — [docs/AI_PIPELINE_AUDIT.md L387-518](docs/AI_PIPELINE_AUDIT.md)
- Portability Phase 3 (redistributable package: `windeployqt`, `Package.ps1`, first-run model detection) "not started"; cross-platform "out of scope" — [docs/PORTABILITY.md](docs/PORTABILITY.md)
- Computer control: learned policy not qualified; "What has not been established: that any of this generalises beyond the controlled fixture" — [docs/COMPUTER_CONTROL.md L531-535](docs/COMPUTER_CONTROL.md), [docs/COMPUTER_CONTROL.md L767-768](docs/COMPUTER_CONTROL.md)
- Web demo not deployed; voice for web demo is future work — [docs/WEB_DEMO_EVIDENCE.md L65](docs/WEB_DEMO_EVIDENCE.md), [docs/WEB_DEMO.md](docs/WEB_DEMO.md)

### Inferences
- Several docs lag the code (ROADMAP's deterministic history summary, Phase 6 "only ContinueGoal executes", COMPUTER_CONTROL's "Expert is not on-demand", PORTABILITY's "configured Qwen3-VL 8B" as chat model). A comparison report should prefer README + settings.json + recent commits where they disagree. (inferred)

### Gaps
- No issue tracker export or dated milestone plan beyond ROADMAP was found.

---

## Hardware assumptions and resource planning

### Takeaway
Developed and live-verified only on a Windows 11 desktop with RTX 5070 + RTX 2070 Super (Ryzen 9 3900-class CPU); Windows 10 1809+ x64 only. A startup `ResourcePlanner` places chat on the strongest GPU and voice/STT on the secondary, embeddings on CPU, with VRAM/RAM reserves and idle-admission rules; single-GPU/CPU-only paths are designed but not physically verified.

### Cited Findings
- Dev PC: RTX 5070 + RTX 2070 Super, Windows 11 — [README.md L13](README.md); CPU "Ryzen 9 3900-class" — [docs/TTS_PERFORMANCE.md L3](docs/TTS_PERFORMANCE.md)
- OS: Windows 10 1809+ 64-bit; links `sapi`, `dxgi`, `gdiplus`; UI Automation; "Cross-platform support is out of scope" — [README.md L86](README.md), [docs/PORTABILITY.md](docs/PORTABILITY.md)
- Setup profiles: Minimal ~3.7 GB (chat, memory, STT; one modest GPU or CPU), Standard ~4.4 GB (+ screen vision), Full ~10.7 GB (+ 8B Expert, best with 12 GB+ total VRAM); Qwen voice adds ~5 GB env + 2–5 GB models; leave 30 GB free — [README.md L88-96](README.md), [Config/model_manifest.json](Config/model_manifest.json)
- Manifest VRAM guidance: Main 4600 MiB, main mmproj 700, Expert 7000, Expert mmproj 800, STT 800/1000, Fast and embeddings 0 (CPU) — [Config/model_manifest.json](Config/model_manifest.json)
- Accelerators: CUDA (NVIDIA; CUDA 13.3 for RTX 50-series, 12.4 earlier), Vulkan (AMD/Intel), CPU; whisper.cpp has no Windows Vulkan build so Vulkan machines get OpenBLAS CPU — [docs/PORTABILITY.md](docs/PORTABILITY.md)
- Resource settings: `assignments` chat `auto-primary`, voice `auto-secondary`, speechRecognition `auto-secondary`, embeddings `cpu`; `gpuReserveMiB` 1536; `minimumFreeRamMiB` 4096; `reserveLogicalCores` 2; `allowChatModelSplit` false; `qwenMinimumFreeVramMiB` 4600; `autoFitTargetMiB` 2048 — [Config/settings.json](Config/settings.json)
- Planner behavior: inventories llama.cpp-reported devices, keeps chat/vision on highest-capacity device when it fits, splits layers only when needed; TTS starts loading once chat has fit its layers — [docs/ARCHITECTURE.md L216](docs/ARCHITECTURE.md)
- Idle background admission: background work admitted when pressure is only VRAM, >= 512 MiB remains per pressured card, engine activity <= 55%; stabilized over three samples — [docs/ARCHITECTURE.md L296-315](docs/ARCHITECTURE.md)
- "On one GPU, or none, the resource planner moves work to what is available instead of switching features off" — [README.md L360](README.md)
- Startup: README says ~10 s to ready plus ~20 s voice warm — [README.md L26](README.md); elsewhere "about 26 seconds" workers + ~40 s voice — [README.md L179-180](README.md); ROADMAP "26–31s" Qwen warmup — [docs/ROADMAP.md L249](docs/ROADMAP.md) (internal inconsistency; likely different dates)
- A single-GPU RTX 3070 Laptop (8 GB) result is recorded: auto-placement kept voice on CPU; pinning both chat and voice to CUDA left 471 MiB free, chat startup 7.5 s → 36.5 s, short phrase 24.13 s — [docs/TTS_PERFORMANCE.md L15-20](docs/TTS_PERFORMANCE.md) (conflicts with README's "only been run on the dev PC", [README.md L39](README.md))
- Low-end limits: Qwen3-TTS on CPU "not usable in real time"; vision on CPU "slow enough to feel broken"; 0.8B on CPU needs 8–10 s just to read a prompt — [docs/PORTABILITY.md](docs/PORTABILITY.md), [README.md L48](README.md)
- Roadmap rules: "Do not fill VRAM to 100% or assume more GPUs are automatically faster"; "Do not hard-code the development machine into the product" — [docs/ROADMAP.md L279-280](docs/ROADMAP.md)

### Inferences
- The practical floor for the "full Revia" experience (Main 4B on GPU + Qwen voice) is roughly two GPUs or one 12 GB+ card; an 8 GB single-GPU laptop gets chat on GPU with CPU/SAPI voice. (inferred from the settings and the 3070 Laptop result)
- The architecture's multi-process design makes swapping a model (e.g., to a newer small model in GGUF) a config change, but swapping to a non-llama.cpp runtime is code work. (inferred)

### Gaps
- No physical CPU-only or AMD/Vulkan run is recorded.
- No long-duration (hours) resource leak data is recorded.
