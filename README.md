# R.E.V.I.A

R.E.V.I.A is a modular C++ desktop assistant. Its Qt shell combines local llama.cpp chat and multimodal vision, hybrid structured memory, affect-aware Windows speech, toggled CUDA whisper.cpp speech recognition, and supervised filesystem or Windows UI Automation actions. Every OS action still passes through deterministic policy, confirmation, dispatch, and JSONL audit boundaries.

The runtime is multi-worker rather than a single blocking pipeline: UI, interactive inference, TTS, STT, and background memory each have independent lifecycle and cancellation ownership. Long-running goals and avatar rendering remain later work; neither should require giving the model unrestricted shell or administrator access.

## What works now

- Local llama.cpp chat through its OpenAI-compatible chat-completions endpoint.
- A Qt 6 desktop window with horizontal component status and chat controls plus matching Chat, Activity, Voice Studio, and Settings tabs, stop control, always-on-top option, and tray controls.
- A bounded affect controller for Revia's own response posture (neutral, curious, pleased, concerned, focused, or confused), with hysteresis, decay, reasons, and visible intensity. It does not infer or invent the user's emotions. The posture is **fed into the model** as a system line each turn, so it colours tone and pacing rather than only driving the status chip and the speech rate. It is phrased as Revia's own stance and explicitly forbids her from naming it, describing her feelings, or assuming anything about the user's.
- A collapsible **Thought process** line under each reply. It carries the posture that shaped the turn, any reasoning the model emitted in `<think>` tags, whether the reply was spoken in fragments, and where the time went. Reasoning is stripped from the reply itself, so it is never read aloud and never shown inline as if it were an answer.
- Profile-aware speech on a dedicated worker. Windows SAPI remains the zero-setup fallback; Qwen3-TTS can design a reusable local character voice, clone it for replies, preview it, and assign it independently to each profile. Markdown, code blocks, and raw URLs are removed before speaking, and the UI reports model loading, generation, playback, fallback, and timing.
- Hold-to-talk microphone input using 16 kHz mono PCM and a local CUDA whisper.cpp `small.en` model. Transcripts are placed in the input box for review before sending, while capture and transcription timings remain visible.
- Opt-in full virtual-desktop capture and local Qwen3-VL analysis. Every capture requires confirmation, stays inside llama.cpp's allowed media directory, and is deleted after the request.
- Opt-in ambient window awareness (Stage 6 Tier 0): foreground changes, window creation, and title changes via `SetWinEventHook`. Event-driven, so it costs nothing while the desktop is idle — no polling, no capture, no pixels, no model. **Off by default.** A deny list for password managers, banking, and private browsing suppresses matching windows entirely rather than recording them redacted, an unidentifiable application is denied rather than recorded, and observations are debounced and rate limited so a title that changes per keystroke cannot become a transcript. A status chip is always visible so the capability's absence can never be mistaken for it being off, `/perception pause` stops observation without stopping Revia, and only counts — never titles — are written to the log. Observations roll into a bounded in-memory session history, so `/perception history 60` can describe the last hour without a model or a screenshot; it is capped, never persisted, and `/perception forget` clears it.
- Typed Windows UI Automation for inspecting/focusing an approved app and setting/invoking named controls. Read-only inspection may auto-run; interaction requires confirmation in supervised mode.
- A reusable `ReviaSession` core shared independently of terminal input, with thread-safe runtime events and cooperative cancellation of active inference.
- An automatic online greeting after the configured model becomes ready; normal greetings are answered by the model.
- Automatic structured memory selection without requiring a command: durable identity, preferences, goals, projects, relationships, and constraints are stored in `Memory/revia_memory.db` while transient chat and sensitive credentials are ignored.
- Hybrid semantic memory retrieval: SQLite FTS5/BM25 handles exact terms while a dedicated Nomic embedding model handles paraphrases; reciprocal-rank fusion selects a small relevant set instead of injecting every memory. Existing `revia_memory.jsonl` records are imported once and backfilled in the background.
- An explicit per-turn agent coordinator: `ConversationAgent` owns the latency-sensitive reply, then `MemoryAgent` evaluates durable memory in the background without delaying that reply. Action planning remains a separate typed path behind the capability policy.
- Direct, typed filesystem actions: list, read text, create directory, copy file, move, rename, and move to the Windows Recycle Bin.
- One-action natural-language planning with `/plan <task>` when llama.cpp is running.
- Bounded multi-step goals with `/goal <task>`: the model plans act/verify steps, the plan is **rehearsed against a disposable copy first**, and only a plan that verified there is offered for approval — with that result attached, so approval rests on an observed outcome rather than on plausible-looking text. Each step must then prove it happened before the next begins, and action/retry/time budgets stop a run that is not converging. `/goals` lists them and `/goals resume <id>` continues one an earlier process left unfinished. These are desktop-session commands; the CLI does not have them yet.
- Reviewed learning with `/review`: Revia draws conclusions from what actually happened — goal outcomes and whether her unprompted proposals were accepted or dismissed — and offers them with the evidence attached. **Nothing is remembered until you approve it**, and approving one writes an ordinary preference memory. A lesson can never change a capability, a budget, or a policy; the single automatic adjustment in the system (halving the initiative rate below a precision floor) is computed from counted outcomes, not inferred here. Patterns need at least four finished goals or judged proposals, unfinished goals are not counted as outcomes, and the review will conclude that speaking first is unwelcome when that is what the numbers say.
- Approved-root enforcement for both source and destination paths.
- Windows reparse-point/symbolic-link escape checks.
- Supervised confirmation for writes; read-only actions can run automatically.
- Dry-run support through structured `/action` JSON.
- An `approved_scope` mode for later unattended work with an explicit risk ceiling.
- Read, listing, and affected-entry limits.
- Append-only JSONL action auditing.
- CTest coverage for policy escapes, parsing, dispatcher gates, filesystem limits, dry runs, audit output, and fail-closed configuration.

## Build and run

Requirements: Windows 10 1809 or newer, x64. See [docs/PORTABILITY.md](docs/PORTABILITY.md) for the full story, including CPU-only machines.

**Qt is required for the desktop shell and is not bundled.** Without it the build still succeeds but produces the CLI only. On a clean machine, install it first:

```powershell
Set-Location 'C:\Users\USER\Documents\GitHub\R.E.V.I.A'
.\Tools\InstallQt.ps1
```

That installs Qt 6.8.3 `mingw_64` plus Qt's own CMake, Ninja, and MinGW 13.1.0 packages, so CLion is not required. It needs Python 3.9+ on PATH. If you already have Qt, skip the script and pass the kit explicitly: `cmake --preset debug -DREVIA_QT_ROOT=C:/Qt/6.8.3/mingw_64`. The MinGW kit is mandatory; an MSVC kit is ABI-incompatible with this build.

Then build and run:

```powershell
.\Tools\Build.ps1
.\build\debug\ReviaDesktop.exe
```

The terminal interface remains available as a fallback:

```powershell
.\build\debug\R_E_V_I_A.exe
```

The build script resolves CMake, Ninja, and MinGW from PATH, then a Qt Tools installation, then CLion's bundled copies; configures `build/debug`; builds both interfaces; deploys the Qt runtime beside the desktop executable; and runs CTest. It prints which toolchain it selected and warns if `ReviaDesktop.exe` was not produced. Pass `-SkipTests` only when you deliberately want a build without verification. Configure with `-DREVIA_REQUIRE_DESKTOP=ON` to make missing Qt a hard error instead of a warning.

Large runtimes and models are ignored by Git. On a new PC, run these installers once before starting Revia:

```powershell
.\Tools\DownloadEmbeddingModel.ps1
.\Tools\InstallLlamaCpp.ps1
.\Tools\InstallWhisper.ps1
.\Tools\InstallVisionProjector.ps1
.\Tools\InstallQwenTTS.ps1
```

`InstallLlamaCpp.ps1` and `InstallWhisper.ps1` detect the accelerator on this machine and install the matching pinned build: CUDA on NVIDIA, Vulkan on AMD and Intel, or a CPU build otherwise. That is roughly 1.25 GB on NVIDIA versus about 40 MB on a machine with no usable GPU. Override with `-Accelerator cpu|vulkan|cuda`. `InstallQwenTTS.ps1` pulls PyTorch and is only worth running if you want voice cloning; Windows SAPI is the zero-setup fallback.

The scripts install pinned Windows CUDA builds of llama.cpp and whisper.cpp, Nomic Embed Text v1.5, Whisper `small.en`, the exact Q8 multimodal projector for the configured Qwen3-VL model, and an isolated Python 3.12 Qwen3-TTS runtime. The native/model artifact installers validate pinned SHA-256 values; the speech installer pins its PyTorch and Qwen package versions, while official Qwen model weights use the Hugging Face cache. The main chat GGUF is not downloaded automatically because it is large and model choice is personal; place it at the `modelPath` in `Config/settings.json`.

The executable changes its working directory to its own folder. CMake copies `Config/` beside it after every build, so runtime configuration is read from `build/debug/Config/`. Runtime memories and action audit records are also kept beside the executable.

The llama.cpp server is optional for direct action commands. It is required for chat, vision, and `/plan`. Revia verifies `/health`, the loaded model identity, effective context/slots, and the vision modality before trusting an existing process.

With `autoStartServer` enabled, Revia first checks `/health` and launches the configured `serverExecutable` only when that endpoint is unavailable. Each owned chat and embedding server receives a fresh cryptographically random API key for that run; all health and inference requests use Bearer authentication. The default `autoTune` mode detects dedicated VRAM and system RAM, selects one conservative chat context, and lets llama.cpp fit GPU layers with automatic flash attention while retaining a 1 GiB VRAM margin. The current tiers range from 4,096 tokens on low-memory systems through 65,536 on systems with at least 24 GB VRAM and 64 GB RAM. Because Revia serializes conversation and memory inference, it uses one chat slot instead of dividing the context between unused slots.

`autoMaxTokens` derives the response allowance from one quarter of the effective context reported by llama.cpp, with a 512-token minimum and `maxTokens` as the ceiling. The default ceiling is 4,096. An explicit profile `maxTokens` override disables that adaptive calculation for that profile. Set `autoTune` to `false` to use the configured `contextSize` and `parallelRequests` manually. A server that was already running keeps its original launch settings, although Revia still reads its effective context. If `shutdownServerOnExit` is enabled, an owned child is attached to a Windows kill-on-close job; an existing server is left alone. Server output is written under `Logs/llama-server.stdout.log` and `Logs/llama-server.stderr.log`.

Revia's timestamped application log is written beside the executable at `Logs/revia.log`. The desktop activity panel receives the same timestamped lifecycle and timing events. Each chat turn records query embedding, memory retrieval, prompt assembly, request preparation, inference queue wait, wait-to-first-token, decoding after the first token, and total turn time. The `slowest` field identifies the longest non-overlapping stage. Startup, shutdown, automatic-memory classification, document embedding, and memory database saves are timed as well.

Qwen3-VL handles chat/vision; it does not create audio. Voice Studio uses the separate official Qwen3-TTS family: the 1.7B VoiceDesign model turns a natural-language voice description into a reference WAV, then the 0.6B Base model clones that reference for previews and replies. Presets and profile assignments are stored in `RuntimeData/Voices/voices.json`, not in the checked-in personality profile, so rebuilding does not erase UI choices. The authenticated worker binds only to `127.0.0.1:8092`, loads one speech model at a time, caches extracted clone prompts, and writes diagnostics to `Logs/qwen-tts.stdout.log` and `Logs/qwen-tts.stderr.log`. The first Create voice and Generate preview operations download their model weights from Qwen's official Hugging Face repositories.

`speech.qwenDevice` defaults to `auto`. The speech worker picks CUDA only when free VRAM meets `qwenMinimumFreeVramMiB` at the moment the model loads, so it must not be starved by llama.cpp's GPU fit. Revia used to guarantee that by loading the clone model *before* starting llama.cpp, which put the full 20-70 second speech model load on the path to the first message. Instead, when the active profile has a Qwen voice assigned, that same budget is added to llama.cpp's `--fit-target`, so llama.cpp leaves the voice's VRAM free and the clone model loads in the background once chat is already usable. Startup drops from roughly 47 s to 18 s on the reference machine with no change to the fitted chat context, and the voice reports `Loading` then `Ready` in the status strip as it arrives. If the free-VRAM threshold is still not met the worker uses CPU, and if Qwen synthesis fails, that utterance falls back to Windows SAPI. Replies requested before the load completes queue on the speech worker rather than blocking chat.

Semantic memory uses a second llama.cpp process on port `8081`, launched with `--embedding --pooling mean`. Its `device` setting defaults to `none`, keeping the small embedding model on CPU so it cannot contend with interactive CUDA inference. Query and document prefixes are configured separately. If this server or its model is unavailable, chat continues with SQLite FTS retrieval and `/status` reports the degraded state. Embedding output is stored inside `Memory/revia_memory.db`; no separate vector JSON file is used.

The current agents are specialized tasks, not long-running autonomous workers. The UI remains responsive on its own thread, visible operations run on a cancellable worker, the memory agent has a background queue, and chat plus embedding use separate llama.cpp processes. Interactive conversation is intentionally prioritized instead of competing with memory classification for the same model slot. Long-running goal agents still belong behind a supervisor with cancellation, budgets, health, and the existing action policy.

## Commands

Paths containing spaces must be quoted.

```text
/help
/status
/backend
/capabilities
/plan create a folder named Notes in my Revia sandbox
/goal make a Reports folder in my sandbox and copy summary.txt into it
/goals
/goals resume goal-000001
/perception
/perception pause
/perception resume
/perception history 60
/perception forget
/initiative
/initiative accept
/initiative dismiss
/review
/review accept lesson-verification
/bargein
/bargein off
/list "C:\Users\USER\Documents\ReviaSandbox"
/read "C:\Users\USER\Documents\ReviaSandbox\notes.txt"
/mkdir "C:\Users\USER\Documents\ReviaSandbox\New Folder"
/copy "C:\...\source.txt" "C:\...\copy.txt"
/move "C:\...\source.txt" "C:\...\destination.txt"
/rename "C:\...\old.txt" "C:\...\new.txt"
/trash "C:\...\unwanted.txt"
/inspect-window "notepad.exe" "Untitled"
/focus-window "notepad.exe" "Untitled"
/set-text "notepad.exe" "Untitled" "Text editor" "Hello from Revia"
/invoke-control "notepad.exe" "Untitled" "Save"
/action {"action":"create_directory","path":"C:\\Users\\USER\\Documents\\ReviaSandbox\\Preview","dry_run":true}
```

Every proposal—including one produced by the LLM—must pass through the same parser, policy, dispatcher, and audit path. Unsupported actions such as shell execution are rejected.

## Capability configuration

The source configuration is `Config/capabilities.json`. The safe default is:

- `mode: supervised`
- only `%USERPROFILE%\Documents\ReviaSandbox` is approved
- only `notepad.exe` and `explorer.exe` are approved for typed UI Automation by default
- read-only work can run without a prompt
- filesystem writes require confirmation

After changing the source configuration, rebuild so the post-build copy updates `build/debug/Config/`.

For a later unattended experiment, use a dedicated disposable folder and set:

```json
{
  "mode": "approved_scope",
  "approvedRoots": ["%USERPROFILE%\\Documents\\ReviaSandbox"],
  "autoApproveRiskThrough": "reversible_write"
}
```

## Speaking first, and being interrupted

Two halves of the same thing: Revia can start a conversation, and can be stopped mid-sentence the way a person can.

**Barge-in** is on by default and costs nothing when idle. The microphone is armed only for the duration of each spoken reply — not whenever Revia is running — and only measures frame energy; nothing is transcribed or stored by the monitor. When it fires, Revia stops talking and starts listening. It stops playback without cancelling the Qwen request that produced the audio, because killing that worker would make the next reply pay a full model reload.

The hard part is that **the microphone hears the speakers for the whole utterance**, not just its opening moments — so a fixed threshold cannot separate "the user is talking" from "Revia is talking and the room is echoing it back". An earlier version used a fixed threshold with a short grace window and consequently interrupted itself about a second into every reply. Detection now learns a noise floor during `startupGraceMs` — which is precisely when it can measure what Revia's own playback sounds like through this microphone — and then requires `echoMarginMultiplier` times that floor, sustained across `consecutiveFramesRequired` frames of roughly 50 ms each. Frames that qualify never update the floor, or someone talking steadily would teach the detector to ignore them. `energyThreshold` remains an absolute floor so a silent microphone cannot produce a hair trigger.

If it still misfires on your hardware, `/bargein off` disarms it immediately, mid-reply, without editing config or restarting. Raise `echoMarginMultiplier` for loud speakers or a microphone close to them.

**Text and speech are synchronised.** A reply that is going to be spoken is held until its audio actually starts, so the words appear as Revia says them rather than several seconds ahead. A reply that will *not* be spoken — speech disabled, a command result, a system message — is shown immediately and never waits. If speech fails, is disabled mid-flight, or is interrupted, the text is released at once; a nine-second timer guarantees a reply is never lost to a stalled voice.

**Initiative is off by default.** When enabled, Revia may offer an observation unprompted, but the decision to speak is deterministic policy rather than a model's opinion of its own interestingness — the same reason a model does not choose its own capability scope. A proposal must clear a *confidence* threshold, not a relevance one, and is then subject to:

- a cooldown after every utterance, and a much longer one after a dismissal — being told "no" costs more than being ignored
- an hourly ceiling
- hard suppression while a full-screen application is in front, while an excluded application is in front, and while the user is mid-input (measured from `GetLastInputInfo`, which reports *when* the last input happened and never what it was, so there is no keyboard hook)
- refusal to repeat an observation already offered

Evidence sources are ranked by how concrete they are. An **unfinished goal outranks a session observation**, because a goal is something you actually asked for while time spent in an editor is only an observation about it; accepting a goal-backed proposal hands it straight to the runner, which re-verifies every remaining step. Every proposal carries the evidence behind it, so the reasoning can be judged rather than just the conclusion, and `/initiative dismiss` answers it in one action. Revia tracks its own precision — accepted over judged — and **halves its own hourly rate** when it falls below `minimumPrecision`, recovering it when proposals start landing. An assistant that cannot tell it is being annoying is a defect. Accepting a proposal that names a goal hands it to the Stage 4 runner, which rehearses, confirms, budgets, and audits exactly as for a typed `/goal`; accepting adds no authority, it only saves the typing.

## Ambient perception

`Config/settings.json` carries a `perception` section, disabled:

```json
{
  "enabled": false,
  "minimumEventIntervalMs": 750,
  "maxObservationsPerMinute": 60,
  "excludedApplications": [],
  "excludedTitleFragments": []
}
```

Setting `enabled` to `true` is the only opt-in; it is deliberately separate from vision's per-capture consent, because continuous observation is a different question from one confirmed screenshot.

The two exclusion arrays **extend** the built-in deny list rather than replace it, so a config that adds one password manager cannot silently drop the rest. The built-ins cover common password managers plus title fragments for banking, private browsing, recovery phrases, and authenticator codes. A window that matches produces no observation at all — it is counted as excluded and nothing about it is written anywhere, because "switched to the bank at 14:02" is itself the disclosure. An application whose executable cannot be identified is treated as excluded, not as unknown-and-therefore-fine.

`minimumEventIntervalMs` debounces from the last *admitted* observation, not the last event, so an editor that rewrites its title on every keystroke yields one observation rather than a transcript. `maxObservationsPerMinute` is a rolling budget on top of that. Both fail closed at load: an interval under 100 ms or a budget over 600 is rejected rather than clamped.

Observations reach the activity feed and the runtime event bus. Only counts reach `Logs/revia.log` — never titles. `/perception` reports the current state and those counts, and `/perception pause` stops observing without stopping Revia; the hook stays installed so resuming is immediate.

Observations are also rolled into a bounded session history, so `/perception history 60` answers what the last hour was spent on:

```text
In the last 60 minutes:
  43m  code.exe
      reviaSession.cpp, goalRunner.cpp
  5m  msedge.exe (2 visits)
      docs
```

Consecutive observations of one application merge into a span, and a span runs until the *next* application appears rather than until its own last window event — an application used quietly generates no events, and reporting no time for forty minutes of reading would answer the question wrongly. That attribution is capped at five minutes, so a machine left alone overnight does not credit all of it to whatever happened to be in front.

The history is **in memory only and never written to disk.** It is capped at 240 spans and eight hours, keeps at most four distinct titles per span, and is discarded when Revia stops. `/perception forget` clears it immediately. A window-title history is exactly the kind of thing that should not quietly become a file as a side effect of enabling a status chip.

## Capability notes

Keep roots and executable names narrow. `approved_scope` does not mean unrestricted PC control: actions outside approved roots/apps or above the risk ceiling remain blocked, no confirmation prompt can override that block, and all outcomes are audited. Control text values are not copied into the audit log; only their length is recorded.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for module boundaries, [docs/PORTABILITY.md](docs/PORTABILITY.md) for build requirements and low-end machine behaviour, and [docs/ROADMAP.md](docs/ROADMAP.md) for the staged path to a desktop companion and supervised autonomy.
