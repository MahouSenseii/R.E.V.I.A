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
- perform a narrow set of confirmed filesystem and Windows UI Automation actions; and
- show exactly which models, GPUs, queues, timings, and errors are active.

The long-term goal is still simple even though the internals are not: **one Revia**. Future Fast, Main, and Expert models should feel like the same person using different amounts of effort—not three assistants taking turns.

## What works today

### Conversation and personality

- Ordinary chat uses a local Qwen3.5 4B model through llama.cpp.
- Fast conversation disables extended thinking, streams immediately, and uses a smaller reply budget. Explicit debugging, architecture, or planning requests can use the deeper response budget.
- The same personality, affect, recent conversation, relevant memories, and current desktop context are assembled for each turn.
- Deterministic response checks remove control tokens, prompt leakage, fabricated runtime state, and known repetitive reply failures before text reaches speech or memory.
- Mood has momentum instead of resetting every turn. Revia can also develop durable likes, dislikes, opinions, and relationship judgments.
- Conversation history and structured memories use separate SQLite stores. Sensitive-looking turns are withheld rather than quietly saved in redacted form.

### Seeing, listening, and speaking

- Continuous awareness is event-driven. Filtered window changes wake a bounded local analysis of the complete virtual desktop, including every monitor and its real coordinates.
- Temporary screenshots are deleted immediately. Only a short in-memory summary is retained, and screen text is treated as untrusted content rather than instructions.
- User input and queued speech preempt background vision. `/perception pause` stops observation immediately; `/perception forget` clears the in-memory activity and visual summaries.
- The old **Analyze screen** button has been removed. **Use screen** still exists for a specific, confirmed UI action because observation and action authority are deliberately separate.
- Speech recognition uses a persistent local Distil-Whisper service with hold-to-talk or optional hands-free VAD.
- Qwen3-TTS splits long replies into short phrases at safe sentence, clause, or word boundaries. Different GPUs may finish phrases out of order, but an explicit sequence gate always plays them in the original order.
- Barge-in lets the user interrupt a spoken reply without unloading the voice model.

### Research, initiative, and actions

- Internet grounding is opt-in. The visible browser uses a dedicated profile, blocks downloads and private/local destinations, and exposes the exact query, sources, and bounded text returned to Revia.
- Local screen-context questions stay local; asking “what am I doing on my screens?” does not become a web search.
- Curiosity and initiative are bounded background lanes. They can nominate silence, a short opening, or one read-only research query, but user work always has priority.
- Filesystem actions, goal steps, and Windows UI Automation all pass through typed parsers, capability policy, confirmation rules, rate limits, and JSONL audit logging.
- The model never gets unrestricted shell access or coordinate-click authority.

### Developer visibility

- The Qt desktop groups activity, pipelines, resources, permissions, voice, canvas, presence, and settings without hiding worker state.
- Startup and per-turn logs include model placement, wait-to-first-token, decoding, memory, vision, voice, and shutdown timings.
- Resource readings show actual system GPU memory, Revia-owned process memory, and CPU load beside the startup budget.
- The CLI exposes the same core runtime for testing and recovery.

## Models Revia actually uses now

Having a model file in `Models/` does not mean it is active. This is the current runtime map:

| Job | Active model | Typical placement on the development PC |
|---|---|---|
| Chat and normal vision | `Qwen3.5-4B-Q4_K_M.gguf` | RTX 5070 through llama.cpp |
| Vision projector | `mmproj-F16.gguf` | Loaded with the 4B chat model |
| Semantic memory | `nomic-embed-text-v1.5.Q4_K_M.gguf` | CPU in a separate llama.cpp worker |
| Speech recognition | `ggml-distil-small.en.bin` | RTX 2070 Super through whisper.cpp |
| Reply voice cloning | `Qwen3-TTS-12Hz-0.6B-Base` | RTX 5070 BF16 + RTX 2070 Super FP32 |
| Voice creation | `Qwen3-TTS-12Hz-1.7B-VoiceDesign` | Loaded only when creating a voice |

The installed 0.8B, 8B, Omni, and other GGUF files are not silently used for normal chat. The planned Fast/Main/Expert router is described in [the roadmap](docs/ROADMAP.md); it must be implemented and benchmarked before those models join the live conversation path.

Qwen TTS and llama.cpp appear as local HTTP workers because separate processes give each GPU its own CUDA context and make crashes, cancellation, and shutdown easier to contain. They bind only to `127.0.0.1`; this is local process communication, not cloud inference.

## Current two-GPU behavior

On the RTX 5070 + RTX 2070 Super development machine, Auto currently resolves to:

| Work | Device |
|---|---|
| Chat and vision | RTX 5070 |
| First/latency-sensitive TTS phrase | RTX 5070 |
| Phrase-ahead TTS and Whisper | RTX 2070 Super |
| Embeddings and SQLite retrieval | CPU |

The 5070 voice worker uses BF16. The 2070 Super uses FP32 because this Qwen3-TTS build is not stable in FP16 on Turing and Turing has no native BF16. Short replies use one worker; longer replies create enough independent phrase jobs for both cards.

The active phrase target is 64 characters. In the latest local comparison, a roughly 100-character first phrase that previously took 45.4 seconds was split so first-phrase synthesis completed in 18.1 seconds. Other phrases in that run completed in roughly 9–20 seconds. This is a useful improvement, not instant speech: model warmup still takes about 34–41 seconds after startup, and `flash-attn` is not installed in the Qwen TTS environment.

Revia does not require two GPUs. The resource planner can fall back to one GPU or CPU and changes placement rather than disabling the assistant. Physical laptop and CPU-only validation are still listed as verification work in the roadmap.

## Build and run

R.E.V.I.A currently targets 64-bit Windows 10 1809 or newer. See [Portability](docs/PORTABILITY.md) for lower-end and non-NVIDIA behavior.

### 1. Install Qt for the desktop app

Qt is optional for the CLI but required for `ReviaDesktop.exe`:

```powershell
Set-Location 'C:\path\to\R.E.V.I.A'
.\Tools\InstallQt.ps1
```

The script installs the pinned Qt 6.8.3 MinGW kit and its CMake/Ninja toolchain. An existing Qt kit can be supplied with `-DREVIA_QT_ROOT=C:/Qt/6.8.3/mingw_64`.

### 2. Install local runtimes and models

Run the pieces you need:

```powershell
.\Tools\InstallLlamaCpp.ps1
.\Tools\DownloadEmbeddingModel.ps1
.\Tools\InstallVisionProjector.ps1
.\Tools\InstallWhisper.ps1
.\Tools\InstallQwenTTS.ps1
.\Tools\DownloadRuntimeModels.ps1 -IncludeBackgroundModel
```

The installers are designed to preserve valid existing files, resume downloads where supported, pin known versions, and validate downloaded artifacts. Qwen voice cloning is optional because Windows SAPI remains the no-setup fallback.

### 3. Build, test, and start

```powershell
.\Tools\Build.ps1
.\build\debug\ReviaDesktop.exe
```

CLI fallback:

```powershell
.\build\debug\R_E_V_I_A.exe
```

`Build.ps1` configures `build/debug`, builds available targets, deploys Qt beside the desktop executable, synchronizes `Config/`, and runs CTest. Pass `-SkipTests` only when you intentionally do not want verification.

There is not yet one fully verified `setup.bat` that turns every clean supported PC into a ready installation. That is a roadmap milestone, not a current claim.

## Everyday controls

The desktop UI is the normal interface. These CLI commands are useful for diagnostics and recovery:

| Command | Purpose |
|---|---|
| `/status`, `/backend`, `/resources` | Show model, service, and hardware state |
| `/perception` | Show screen-awareness state and counters |
| `/perception pause`, `resume`, `forget` | Control or clear local awareness |
| `/history <words>`, `/history forget` | Search or clear conversation history |
| `/internet on`, `manual`, `off` | Choose automatic, explicit-only, or no lookup |
| `/web "query"` | Request one web lookup |
| `/bargein`, `/bargein off` | Inspect or disable voice interruption |
| `/initiative`, `accept`, `dismiss` | Review a proactive proposal |
| `/goal <task>`, `/goals` | Rehearse and supervise a bounded multi-step goal |
| `/plan <task>` | Plan one typed action |
| `/draw <description>` | Create a sanitized SVG diagram |
| `/imagine <description>` | Generate a local image when enabled |
| `/write`, `/revise`, `/scene`, `/undo` | Work with the current document |
| `/quality`, `/eval` | Inspect conversation-quality diagnostics |
| `/prefs`, `/set`, `/unset` | Change allowlisted comfort preferences |

Direct file and UI Automation commands remain available through `/help`. Paths containing spaces must be quoted.

## Privacy and safety boundaries

Revia is local-first, but “local” does not mean “unrestricted.”

- The checked-in capability template is supervised and limits filesystem work to `%USERPROFILE%\Documents\ReviaSandbox`.
- Application approval grants inspection only. Invoking or changing a control also requires an exact approved control name or automation ID.
- Filesystem writes and mutable desktop actions require policy approval and normally confirmation.
- Password managers, banking, private browsing, recovery phrases, and similar titles are excluded from ambient perception by default.
- Internet access is separately opt-in because query text leaves the machine when a lookup runs.
- Screen analysis can describe; it cannot click. **Use screen** must resolve the visual target back to a permitted Windows UI Automation element before policy and confirmation are evaluated.
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

- Normal conversation currently uses one 4B brain. Reflex, 0.8B Fast, and 8B Expert routing are planned, not active.
- Qwen3-TTS is local and noticeably faster with short parallel phrases, but it is still not real-time on this hardware.
- Continuous awareness summarizes visible context; it is not a video recorder, OCR archive, or permission to act.
- Autonomous learning is bounded to evidence, read-only research, and reviewable memory. Revia cannot silently edit her own production source or widen her permissions.
- Physical single-GPU laptop, CPU-only, clean-machine setup, extended VRAM-leak, and long cancellation stress tests are not yet fully verified.
- Remote PCs, camera input, and avatar embodiment are designed or planned but not implemented.

## Project docs

- [Architecture](docs/ARCHITECTURE.md) — ownership, worker, policy, and data-flow boundaries.
- [Roadmap](docs/ROADMAP.md) — what is working, what comes next, and what is intentionally later.
- [Portability](docs/PORTABILITY.md) — hardware and build behavior across machine classes.
- [Conversation quality](docs/CONVERSATION_QUALITY.md) — the regression contract for Revia's voice and behavior.

The project’s guiding rule is: **internally, many specialized systems; externally, one Revia.**
