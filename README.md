# R.E.V.I.A

R.E.V.I.A stands for **Reactive Emotional Virtual Interactive Assistant**. She is a local-first Windows desktop companion written in C++: one persistent character with conversation, memory, mood, voice, vision, curiosity, and carefully supervised computer actions.

This is an active personal project, not a finished consumer app. The current priority is making Revia's mind feel fast, consistent, and alive before adding an animated avatar.

## The short version

Revia can currently:

- chat through a local Qwen model and stream replies as they are generated;
- remember useful facts and continue a conversation after a restart;
- keep a persistent mood, preferences, opinions, and relationship context;
- listen through local Whisper and speak through either Qwen3-TTS or Windows SAPI;
- notice useful context across multiple monitors without an Analyze Screen button;
- look things up in a visible, restricted browser when internet access is enabled;
- suggest something on her own when she has real evidence and policy allows it;
- draw diagrams, maintain a working document, and generate images when those features are enabled;
- perform a song from WAV assets you supply, with timed sections and measured sung phrases;
- perform a narrow set of confirmed filesystem and Windows UI Automation actions;
- when the owner turns it on, move the pointer, drag, type, and start applications — either confined to one approved window or across the whole desktop — with a physical emergency stop; and
- show exactly which models, GPUs, queues, timings, and errors are active.

The design rule is simple even though the internals are not: **one Revia**. Reflex, Fast, Main, and Expert share one identity, mood, memory, relationship state, and desktop context; routing changes how much effort she uses, not who she is.

## What works today

### Conversation and personality

- A deterministic C++ Reflex lane handles stop/cancel and direct “Revia?” calls without an LLM.
- Qwen3.5 0.8B handles cheap social turns, Qwen3.5 4B handles normal conversation, and Qwen3-VL 8B handles difficult debugging/architecture and difficult vision. The decision is made before generation and only one conversational model answers a normal turn.
- Ordinary conversation uses a short response budget. Explicit debugging, architecture, or planning requests can opt into deep reasoning, and unavailable tiers fall back visibly instead of failing the turn.
- The same personality, affect, recent conversation, relevant memories, and current desktop context are assembled for each turn.
- Deterministic response checks remove control tokens, prompt leakage, fabricated runtime state, generated User/You turns, known repetitive reply failures, and any unfinished token-limit tail before text reaches speech or memory.
- Mood has momentum instead of resetting every turn. Revia can also develop durable likes, dislikes, opinions, and relationship judgments.
- Conversation history and structured memories use separate SQLite stores. Sensitive-looking turns are withheld rather than quietly saved in redacted form.

### Seeing, listening, and speaking

- Continuous awareness is event-driven with a 30-second idle refresh backstop. Filtered window changes wake a bounded local analysis of the complete virtual desktop, including every monitor and its real coordinates; constantly changing window titles cannot postpone it forever.
- Application and window-title exclusions apply to **activity metadata only**. They suppress matching activity records; they do **not** hide those windows in screenshots sent to vision. Screen capture has its own permission and can still include excluded windows.
- Temporary screenshots are deleted immediately. Only a short in-memory summary is retained, and screen text is treated as untrusted content rather than instructions.
- User input preempts background vision. Voice and vision can continue as separate bounded workers while Revia is otherwise idle, so a long spoken answer no longer makes screen context go stale. `/perception pause` stops observation immediately; `/perception forget` clears the in-memory activity and visual summaries.
- The old **Analyze screen** button has been removed. **Use screen** still exists for a specific, confirmed UI action because observation and action authority are deliberately separate.
- Camera access is opt-in and off by default, behind its own capability in `capabilities.json`. Revia takes a single still frame through Media Foundation and closes the device immediately, so the hardware light is lit only while a frame is being taken. Using the camera when asked and deciding to look on her own are two separate permissions, and revoking the first revokes the second. Frames land in `RuntimeData/Camera` and are never uploaded.
- Speech recognition uses a persistent local Distil-Whisper service with hold-to-talk or optional hands-free VAD.
- Qwen3-TTS begins with the first complete natural sentence. Legacy character targets remain in the configuration, but Revia never cuts speech at a comma or arbitrary word just to create another GPU job. The first sentence stays on the measured latency worker; later complete sentences use another GPU only when its predicted finish will not stall ordered playback.
- The selected clone prompt is prepared during background warmup, both GPU workers warm concurrently, and normal conversation returns a bounded in-memory WAV/PCM buffer to Windows playback instead of writing a temporary file. Saved previews and voice assets still use WAV files.
- The installed Qwen package does **not** provide true incremental audio generation. Revia therefore uses accurately named text-pipelined phrase generation, not fake “streaming audio.”
- Barge-in lets the user interrupt a spoken reply without unloading the voice model.

### Karaoke

Singing is a performer capability, not a long sentence through the TTS queue. It has its
own audio device, its own thread, and its own owner, so a song that fails to load cannot
cost Revia her voice.

Songs live in `RuntimeData/Songs/<name>/`, which is created on first run:

```text
RuntimeData/Songs/my-song/
  instrumental.wav   optional backing track
  vocal.wav          optional vocal track
  song.json          optional title, credit, gains, and timed lines
```

At least one track is required. A folder holding a single `.wav` and no `song.json` is
still a song, so dropping one file in and asking her to sing it works with no
configuration. Two tracks are mixed sample-accurately into one stream — there is no
second player to drift out of time with the first.

| Command | Purpose |
|---|---|
| `/songs` | List what is in the library, including folders that cannot be played and why |
| `/sing <name>` | Perform a song; matches an exact id, or a unique id/title prefix |
| `/sing` or `/sing status` | Position, current section, and current line |
| `/sing check <name>` | Load and mix without playing, to see whether a new asset works |
| `/sing stop` | Stop the performance |

Details worth knowing:

- **One voice.** She stops singing to answer you rather than talking over herself.
  Set `performance.interruptSongToSpeak` to `false` to keep the music running and leave
  the reply on screen only. Stop stops the song along with everything else.
- **Sung phrases are measured, not declared.** `VocalStarted`/`VocalEnded` events come
  from the loudness of the vocal track itself, so an avatar's mouth follows the audio
  rather than a file that claims what the audio does.
- **Both tracks must share a sample rate.** Mismatches are refused with both numbers
  rather than run through a hasty resampler that would sound worse than the refusal.
  Mono is upmixed to stereo; 8/16/24/32-bit PCM and 32/64-bit float are all accepted.
- **Clipping is reported.** `/sing check` says how many samples hit the limit when the
  two tracks are summed, so a mix that is quietly destroying itself says so instead of
  just sounding harsh. Lower `instrumentalGain` or `vocalGain` in `song.json`.
- The library reads that folder and never writes to it or reaches outside it. A song
  name arrives from a chat message, so it is treated as untrusted text and validated as
  a plain folder name before it is ever joined to a path.

Revia performs the audio you put in that folder — the project ships no songs, and
nothing here generates singing. `song.json`'s title, credit, and line text are carried
through as opaque data for display and timing. What goes in the library, and whether you
have the right to perform it, is yours to decide. `ISingingEngine` is the intended
replacement point later: it changes where the vocal samples come from, not the mixing,
timing, or event machinery.

### Research, initiative, and actions

- Internet grounding is opt-in. The visible browser uses a dedicated profile, blocks downloads and private/local destinations, and exposes the exact query, sources, and bounded text returned to Revia. Explicit lookups fall back to allow-listed DuckDuckGo/Wikipedia APIs when the visible results page cannot be extracted; autonomous research remains visible-browser-only.
- Local screen-context questions stay local; asking “what am I doing on my screens?” does not become a web search.
- Curiosity and initiative are bounded background lanes. They can nominate silence, a short opening, or one read-only research query, but user work always has priority.
- Filesystem actions, goal steps, Windows UI Automation, and desktop operation all pass through typed parsers, capability policy, confirmation rules, rate limits, and JSONL audit logging.
- The model never gets unrestricted shell access or coordinate-click authority.

### Driving the desktop

Pointer, keyboard, and application launch are off in the checked-in capability template
and each is granted separately in the **Permissions** tab. There are two scopes, and the
scope is the whole question.

**`approved_applications`** (the default). Every action names an executable on the
approved list. Input is only ever synthesized into a window belonging to that
executable, and the executor re-checks the real foreground window immediately before it
presses anything — if focus moved, nothing is sent. A point must land inside that
window. Windows-key chords and application-switching chords are refused, because leaving
the application is exactly what confinement means. A click aimed at something Revia saw
re-finds that exact UI Automation element and uses its **current** bounds; the
coordinate captured when the plan was made is never the coordinate clicked.

**`whole_desktop`**. The pointer goes anywhere on the virtual desktop and the keyboard
goes to whatever has focus, the way it does for the person sitting there. This exists
because a general skill — move the pointer, see what is under it, press a key, see what
changed — cannot be learned one integration at a time, and Revia is meant to learn the
machine rather than be wired into it.

It is worth being plain about the trade: **confinement is what is given up.** The scope
requires pointer control and permission to aim at chosen coordinates, needs an explicit
answer to a dialog that says so, and is off until then. What remains after it is on:

- **Command surfaces are still refused.** Terminals, script hosts, and `regedit` are
  refused by executable name at the moment of injection, and `win+r`, `win+x`, `win+s`,
  `win+i` are refused as chords. This is the surviving form of "model text never becomes
  a shell command". `allowCommandSurfaces` turns it off, separately and deliberately.
- The per-minute and minimum-interval **input budget**, separate from the UI Automation
  budget.
- The **audit trail** — two records per action, pointer geometry and key chords in full,
  typed text by length only. It is not a keystroke log.
- The **emergency stop**, below.

None of those is a proof. A pointer that can reach any pixel can click a taskbar icon,
and a keyboard that reaches any focused window can drive whatever is in front of it.
That is inherent in the capability, not a gap in it, and it is why the scope is a
deliberate grant rather than a default.

Common to both scopes:

- `launch_application` starts an approved executable with no arguments, or with one file
  that passes the same approved-root checks as any other filesystem action.
- Typed text is bounded in length and may not contain control characters.
- Pointer actions report what is under the pointer afterwards — the accessible name,
  control type, and owning executable — so an action has an observable result rather
  than a silent one. That is description, never permission: nothing about being able to
  name a control lets her press it.
- **Stop:** hold `ctrl+alt+shift`, press Stop in the desktop shell, use the Permissions
  tab button, or run `/desktop stop`. The stop latches; `/desktop resume` clears it. It
  does not travel through the model or the turn queue, and an executor with no stop
  path available refuses to act at all.

Every command has a screen form and a window form. An executable name never parses as a
number, so the two never collide:

```text
/click "820" "140"                              anywhere on screen
/click "notepad.exe" "Untitled" "40" "18"       inside one approved window
/drag  "10" "20" "90" "120" ["button"]
/press "alt+tab"        /press "notepad.exe" "Untitled" "ctrl+s"
/type  "hello"          /type  "notepad.exe" "Untitled" "hello"
/scroll "-4"            /scroll "notepad.exe" "Untitled" "-4"
```

`mode` in the capability file also accepts `owner_full_access`, which is the owner
choosing that reversible in-scope work should stop asking for confirmation. It changes
that ceiling and nothing else: approved roots, approved applications, approved controls,
and every desktop-control switch are evaluated exactly as they are under `supervised`.

### Developer visibility

- The Qt desktop groups activity, pipelines, resources, permissions, profiles, voice, canvas, presence, and settings without hiding worker state.
- **Profiles** shows which profile Revia is running right now, creates and edits profiles as `Config/Profiles/<id>.json`, switches between them, and assigns each one a created voice. **Voice** creates and previews voices; a voice is built once and any profile can be given it.
- Startup and per-turn logs include model placement, wait-to-first-token, decoding, memory, vision, voice, and shutdown timings.
- Resource readings show actual system GPU memory, Revia-owned process memory, and CPU load beside the startup budget.
- The CLI exposes the same core runtime for testing and recovery.

## Models Revia actually uses now

Having a model file in `Models/` does not mean it is active. This is the current runtime map:

| Job | Active model | Typical placement on the development PC |
|---|---|---|
| Reflex | deterministic C++ | In-process; no model or HTTP request |
| Fast conversation | `Qwen3.5-0.8B-Q4_K_M.gguf` | CPU llama.cpp worker on this PC |
| Main chat and normal vision | `Qwen3.5-4B-Q4_K_M.gguf` | RTX 5070 through llama.cpp |
| Main vision projector | `mmproj-F16.gguf` | Loaded with the Main model |
| Expert conversation/vision | `Qwen3-VL-8B-Instruct-Unredacted-MAX.Q4_K_M.gguf` plus its Q8 projector | Separate safely fitted llama.cpp worker; Main Deep fallback on smaller machines |
| Semantic memory | `nomic-embed-text-v1.5.Q4_K_M.gguf` | CPU in a separate llama.cpp worker |
| Speech recognition | `ggml-distil-small.en.bin` | RTX 2070 Super through whisper.cpp |
| Reply voice cloning | `Qwen3-TTS-12Hz-0.6B-Base` | RTX 5070 BF16 + RTX 2070 Super FP32 |
| Voice creation | `Qwen3-TTS-12Hz-1.7B-VoiceDesign` | Loaded only when creating a voice |

The Omni model and the locally identified Llama 3.1 8B Q8 file are not part of automatic routing. Run `/models` to see the exact configured roles, placement, warm/cold state, artifact size, and use counts for the current session.

Qwen TTS and llama.cpp appear as local HTTP workers because separate processes give each GPU its own CUDA context and make crashes, cancellation, and shutdown easier to contain. They bind only to `127.0.0.1`; this is local process communication, not cloud inference.

## Current two-GPU behavior

On the RTX 5070 + RTX 2070 Super development machine, Auto currently resolves to:

| Work | Device |
|---|---|
| Chat and vision | RTX 5070 |
| First/latency-sensitive TTS phrase | RTX 5070 |
| Phrase-ahead TTS and Whisper | RTX 2070 Super |
| Embeddings and SQLite retrieval | CPU |

The 5070 voice worker uses BF16. The 2070 Super uses FP32 because this Qwen3-TTS build is not stable in FP16 on Turing and Turing has no native BF16. Short replies use one worker; longer replies can create enough independent complete-sentence jobs for both cards.

Measured local results are kept under `RuntimeData/Benchmarks/`. The reproducible baseline showed 17/64-character synthesis at 8.72s/15.93s on the 5070 and 9.56s/11.54s on the 2070. Explicit SDPA reduced the 5070 comparison to 7.59s/15.78s, while the 2070 results were mixed, so **Adaptive** uses SDPA only on native-BF16 devices and the stable package default on FP32/Turing devices. Live parallel model-and-prompt warmup fell from roughly 41–50s to 26–31s, and a real queue-to-audible sample fell from 33.1s before this pass to 17.1s afterward. Once generated, a deterministic Reflex phrase is held in a strict nine-entry session cache; a repeated “Okay.” was ready in 46ms and audible in 65ms. Qwen inference remains the bottleneck for new text and measured RTF is still above 1; this is faster and better pipelined, not instant or true incremental speech.

Revia does not require two GPUs. The resource planner can fall back to one GPU or CPU and changes placement rather than disabling the assistant. Physical laptop and CPU-only validation are still listed as verification work in the roadmap.

## Build and run

R.E.V.I.A currently targets 64-bit Windows 10 1809 or newer. See [Portability](docs/PORTABILITY.md) for lower-end and non-NVIDIA behavior.

### One-command setup

From a Windows checkout, the supported bootstrap is:

```powershell
.\setup.bat -Profile Full
```

`Minimal`, `Standard`, and `Full` select pinned artifacts from `Config/model_manifest.json`. The setup verifies/reuses valid files, installs project-local runtimes, builds, tests, and runs the health check. A completely fresh second physical PC is still marked as **not verified**, so setup does not hide failures behind a success message.

### Manual component setup

#### 1. Install Qt for the desktop app

Qt is optional for the CLI but required for `ReviaDesktop.exe`:

```powershell
Set-Location 'C:\path\to\R.E.V.I.A'
.\Tools\InstallQt.ps1
```

The script installs the pinned Qt 6.8.3 MinGW kit and its CMake/Ninja toolchain. An existing Qt kit can be supplied with `-DREVIA_QT_ROOT=C:/Qt/6.8.3/mingw_64`.

#### 2. Install local runtimes and models

Run the pieces you need:

```powershell
.\Tools\InstallLlamaCpp.ps1
.\Tools\DownloadEmbeddingModel.ps1
.\Tools\InstallVisionProjector.ps1
.\Tools\InstallWhisper.ps1
.\Tools\InstallQwenTTS.ps1
.\Tools\DownloadRuntimeModels.ps1 -Profile Full
```

The installers are designed to preserve valid existing files, resume downloads where supported, pin known versions, and validate downloaded artifacts. Qwen voice cloning is optional because Windows SAPI remains the no-setup fallback.

#### 3. Build, test, and start

```powershell
.\Tools\Build.ps1
.\build\debug\ReviaDesktop.exe
```

CLI fallback:

```powershell
.\build\debug\R_E_V_I_A.exe
```

`Build.ps1` configures `build/debug`, builds available targets, deploys Qt beside the desktop executable, synchronizes `Config/`, and runs CTest. Pass `-SkipTests` only when you intentionally do not want verification.

## Everyday controls

The desktop UI is the normal interface. These CLI commands are useful for diagnostics and recovery:

| Command | Purpose |
|---|---|
| `/status`, `/backend`, `/resources`, `/models` | Show model, service, residency, and hardware state |
| `/perception` | Show screen-awareness state and counters |
| `/perception pause`, `resume`, `forget` | Control or clear local awareness |
| `/history <words>`, `/history forget` | Search or clear conversation history |
| `/internet on`, `manual`, `off` | Choose automatic, explicit-only, or no lookup |
| `/web "query"` | Request one web lookup |
| `/bargein`, `/bargein off` | Inspect or disable voice interruption |
| `/songs`, `/sing <name>`, `/sing stop`, `/sing check <name>` | Karaoke from WAV assets |
| `/desktop`, `/desktop stop`, `/desktop resume` | Desktop control state and the emergency stop |
| `/launch`, `/click`, `/drag`, `/move-cursor`, `/scroll`, `/press`, `/type` | Typed desktop operation |
| `/initiative`, `accept`, `dismiss` | Review a proactive proposal |
| `/goal <task>`, `/goals` | Rehearse and supervise a bounded multi-step goal |
| `/plan <task>` | Plan one typed action |
| `/draw <description>` | Create a sanitized SVG diagram |
| `/imagine <description>` | Generate a local image when enabled |
| `/write`, `/revise`, `/scene`, `/undo` | Work with the current document |
| `/quality`, `/eval` | Inspect conversation-quality diagnostics |
| `/self-assessment` | Show evidence-backed latency/failure findings without changing code or settings |
| `/prefs`, `/set`, `/unset` | Change allowlisted comfort preferences |

Direct file and UI Automation commands remain available through `/help`. Paths containing spaces must be quoted.

## Privacy and safety boundaries

Revia is local-first, but “local” does not mean “unrestricted.”

- The checked-in capability template is supervised and limits filesystem work to `%USERPROFILE%\Documents\ReviaSandbox`.
- Application approval grants inspection only. Invoking or changing a control also requires an exact approved control name or automation ID.
- Filesystem writes and mutable desktop actions require policy approval and normally confirmation.
- Password-manager applications and matching sensitive window titles are excluded from ambient **activity metadata** by default. These exclusions do not mask their visible contents in screenshots sent to vision.
- Internet access is separately opt-in because query text leaves the machine when a lookup runs.
- Screen analysis by itself can only describe. Anything Revia does because of what she saw must first resolve back to a permitted Windows UI Automation element, and that element is re-verified at execution time before policy, confirmation, and either a control pattern or a click at its current bounds.
- Action logs omit entered control text and record only its length.

The live capability file is `RuntimeData/Capabilities/capabilities.json`. It is seeded once from `Config/capabilities.json` and is not overwritten by later builds. Use the **Permissions** tab for normal changes.

## Configuration and user data

- Checked-in defaults live under `Config/`.
- CMake copies them beside each executable under `build/debug/Config/`.
- Preferences, voices, captures, evaluation records, and capability state live under `build/debug/RuntimeData/` for that build.
- Structured memory lives in `Memory/revia_memory.db`.
- Conversation history lives in `Memory/revia_conversations.db`.
- Rebuilding does not intentionally overwrite existing memory, voice assignments, preferences, or the live capability file.

The main settings file exposes Auto resource planning, context limits, response budgets, voice phrase length, perception timing, and service paths. Prefer the UI for ordinary settings and edit JSON only when working on the runtime itself.

## Logs and troubleshooting

Start with the **Runtime → Activity**, **Pipelines**, and **Resources** panels. Durable logs are written under `build/debug/Logs/`:

- `revia.log` — startup, turns, model placement, timings, and shutdown;
- `llama-server.*.log` — chat/vision and embedding workers;
- `qwen-tts-<port>.*.log` — one log pair per voice GPU;
- Whisper and browser worker logs when those services are enabled.

`/status` and `/resources` provide the same core facts in the CLI. If a backend device cannot be matched to a Windows adapter, Revia reports its usage as **unmeasured** rather than showing a confidently wrong number.

## Known limitations

- Qwen3-TTS is local and better pipelined, but this installed 0.6B model is still slower than real time on the measured hardware and exposes no true incremental audio API.
- Continuous awareness summarizes visible context; it is not a video recorder, OCR archive, or permission to act.
- Autonomous learning is bounded to evidence, read-only research, reviewable memory, and persisted improvement proposals. Revia cannot silently edit her own production source, change models/settings, or widen her permissions.
- Physical single-GPU laptop, CPU-only, clean-machine setup, extended VRAM-leak, and long cancellation stress tests are not yet fully verified.
- Remote PCs remain planned. Permissioned one-frame camera capture is implemented. Avatar
  character/presence and public-conversation contracts are implemented, but no Live2D/VRM
  renderer, platform connector, model asset, or OBS integration is live-verified yet.

## Project docs

- [Architecture](docs/ARCHITECTURE.md) — ownership, worker, policy, and data-flow boundaries.
- [Roadmap](docs/ROADMAP.md) — what is working, what comes next, and what is intentionally later.
- [Portability](docs/PORTABILITY.md) — hardware and build behavior across machine classes.
- [Conversation quality](docs/CONVERSATION_QUALITY.md) — the regression contract for Revia's voice and behavior.
- [Voice performance](docs/TTS_PERFORMANCE.md) — exact architecture, benchmarks, selected settings, and honest unverified items.

The project’s guiding rule is: **internally, many specialized systems; externally, one Revia.**
