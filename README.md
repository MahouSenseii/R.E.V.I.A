# R.E.V.I.A

R.E.V.I.A is a modular C++ desktop assistant. Its Qt shell combines local llama.cpp chat and multimodal vision, hybrid structured memory, affect-aware Windows speech, CUDA whisper.cpp push-to-talk, and supervised filesystem or Windows UI Automation actions. Every OS action still passes through deterministic policy, confirmation, dispatch, and JSONL audit boundaries.

The runtime is multi-worker rather than a single blocking pipeline: UI, interactive inference, TTS, STT, and background memory each have independent lifecycle and cancellation ownership. Long-running goals and avatar rendering remain later work; neither should require giving the model unrestricted shell or administrator access.

## What works now

- Local llama.cpp chat through its OpenAI-compatible chat-completions endpoint.
- A Qt 6 desktop window with horizontal component status and chat controls plus matching Chat, Activity, Voice Studio, and Settings tabs, stop control, always-on-top option, and tray controls.
- A bounded affect controller for Revia's own response posture (neutral, curious, pleased, concerned, focused, or confused), with hysteresis, decay, reasons, and visible intensity. It does not infer or invent the user's emotions.
- Profile-aware speech on a dedicated worker. Windows SAPI remains the zero-setup fallback; Qwen3-TTS can design a reusable local character voice, clone it for replies, preview it, and assign it independently to each profile. Markdown, code blocks, and raw URLs are removed before speaking, and the UI reports model loading, generation, playback, fallback, and timing.
- Hold-to-talk microphone input using 16 kHz mono PCM and a local CUDA whisper.cpp `small.en` model. Transcripts are placed in the input box for review before sending, while capture and transcription timings remain visible.
- Opt-in full virtual-desktop capture and local Qwen3-VL analysis. Every capture requires confirmation, stays inside llama.cpp's allowed media directory, and is deleted after the request.
- Typed Windows UI Automation for inspecting/focusing an approved app and setting/invoking named controls. Read-only inspection may auto-run; interaction requires confirmation in supervised mode.
- A reusable `ReviaSession` core shared independently of terminal input, with thread-safe runtime events and cooperative cancellation of active inference.
- An automatic online greeting after the configured model becomes ready; normal greetings are answered by the model.
- Automatic structured memory selection without requiring a command: durable identity, preferences, goals, projects, relationships, and constraints are stored in `Memory/revia_memory.db` while transient chat and sensitive credentials are ignored.
- Hybrid semantic memory retrieval: SQLite FTS5/BM25 handles exact terms while a dedicated Nomic embedding model handles paraphrases; reciprocal-rank fusion selects a small relevant set instead of injecting every memory. Existing `revia_memory.jsonl` records are imported once and backfilled in the background.
- An explicit per-turn agent coordinator: `ConversationAgent` owns the latency-sensitive reply, then `MemoryAgent` evaluates durable memory in the background without delaying that reply. Action planning remains a separate typed path behind the capability policy.
- Direct, typed filesystem actions: list, read text, create directory, copy file, move, rename, and move to the Windows Recycle Bin.
- One-action natural-language planning with `/plan <task>` when llama.cpp is running.
- Approved-root enforcement for both source and destination paths.
- Windows reparse-point/symbolic-link escape checks.
- Supervised confirmation for writes; read-only actions can run automatically.
- Dry-run support through structured `/action` JSON.
- An `approved_scope` mode for later unattended work with an explicit risk ceiling.
- Read, listing, and affected-entry limits.
- Append-only JSONL action auditing.
- CTest coverage for policy escapes, parsing, dispatcher gates, filesystem limits, dry runs, audit output, and fail-closed configuration.

## Build and run on this PC

From PowerShell:

```powershell
Set-Location 'C:\Users\USER\Documents\GitHub\R.E.V.I.A'
.\Tools\Build.ps1
.\build\debug\ReviaDesktop.exe
```

The terminal interface remains available as a fallback:

```powershell
.\build\debug\R_E_V_I_A.exe
```

The build script locates the CMake, Ninja, and MinGW tools bundled with CLion, configures `build/debug`, builds both interfaces, deploys the Qt runtime beside the desktop executable, and runs CTest. The desktop target is optional when Qt is absent; this PC uses Qt `6.8.3` from `C:\Users\USER\Qt\6.8.3\mingw_64`. Pass `-SkipTests` only when you deliberately want a build without verification.

Large runtimes and models are ignored by Git. On a new PC, run these installers once before starting Revia:

```powershell
.\Tools\DownloadEmbeddingModel.ps1
.\Tools\InstallLlamaCpp.ps1
.\Tools\InstallWhisper.ps1
.\Tools\InstallVisionProjector.ps1
.\Tools\InstallQwenTTS.ps1
```

The scripts install pinned Windows CUDA builds of llama.cpp and whisper.cpp, Nomic Embed Text v1.5, Whisper `small.en`, the exact Q8 multimodal projector for the configured Qwen3-VL model, and an isolated Python 3.12 Qwen3-TTS runtime. The native/model artifact installers validate pinned SHA-256 values; the speech installer pins its PyTorch and Qwen package versions, while official Qwen model weights use the Hugging Face cache. The main chat GGUF is not downloaded automatically because it is large and model choice is personal; place it at the `modelPath` in `Config/settings.json`.

The executable changes its working directory to its own folder. CMake copies `Config/` beside it after every build, so runtime configuration is read from `build/debug/Config/`. Runtime memories and action audit records are also kept beside the executable.

The llama.cpp server is optional for direct action commands. It is required for chat, vision, and `/plan`. Revia verifies `/health`, the loaded model identity, effective context/slots, and the vision modality before trusting an existing process.

With `autoStartServer` enabled, Revia first checks `/health` and launches the configured `serverExecutable` only when that endpoint is unavailable. Each owned chat and embedding server receives a fresh cryptographically random API key for that run; all health and inference requests use Bearer authentication. The default `autoTune` mode detects dedicated VRAM and system RAM, selects one conservative chat context, and lets llama.cpp fit GPU layers with automatic flash attention while retaining a 1 GiB VRAM margin. The current tiers range from 4,096 tokens on low-memory systems through 65,536 on systems with at least 24 GB VRAM and 64 GB RAM. Because Revia serializes conversation and memory inference, it uses one chat slot instead of dividing the context between unused slots.

`autoMaxTokens` derives the response allowance from one quarter of the effective context reported by llama.cpp, with a 512-token minimum and `maxTokens` as the ceiling. The default ceiling is 4,096. An explicit profile `maxTokens` override disables that adaptive calculation for that profile. Set `autoTune` to `false` to use the configured `contextSize` and `parallelRequests` manually. A server that was already running keeps its original launch settings, although Revia still reads its effective context. If `shutdownServerOnExit` is enabled, an owned child is attached to a Windows kill-on-close job; an existing server is left alone. Server output is written under `Logs/llama-server.stdout.log` and `Logs/llama-server.stderr.log`.

Revia's timestamped application log is written beside the executable at `Logs/revia.log`. The desktop activity panel receives the same timestamped lifecycle and timing events. Each chat turn records query embedding, memory retrieval, prompt assembly, request preparation, inference queue wait, wait-to-first-token, decoding after the first token, and total turn time. The `slowest` field identifies the longest non-overlapping stage. Startup, shutdown, automatic-memory classification, document embedding, and memory database saves are timed as well.

Qwen3-VL handles chat/vision; it does not create audio. Voice Studio uses the separate official Qwen3-TTS family: the 1.7B VoiceDesign model turns a natural-language voice description into a reference WAV, then the 0.6B Base model clones that reference for previews and replies. Presets and profile assignments are stored in `RuntimeData/Voices/voices.json`, not in the checked-in personality profile, so rebuilding does not erase UI choices. The authenticated worker binds only to `127.0.0.1:8092`, loads one speech model at a time, caches extracted clone prompts, and writes diagnostics to `Logs/qwen-tts.stdout.log` and `Logs/qwen-tts.stderr.log`. The first Create voice and Generate preview operations download their model weights from Qwen's official Hugging Face repositories.

`speech.qwenDevice` defaults to `auto`. On a profile with a Qwen voice assignment, Revia loads the clone model before starting llama.cpp; llama.cpp then performs its normal GPU fit around the speech model's real allocation. If the configured free-VRAM threshold is not met, the speech worker uses CPU instead, and if Qwen synthesis fails, that utterance falls back to Windows SAPI. Assigning a voice to the active profile shows a restart reminder so this fitting order can take effect. This makes a future higher-memory GPU use CUDA automatically without hard-coding this laptop's performance.

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

Keep roots and executable names narrow. `approved_scope` does not mean unrestricted PC control: actions outside approved roots/apps or above the risk ceiling remain blocked, no confirmation prompt can override that block, and all outcomes are audited. Control text values are not copied into the audit log; only their length is recorded.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for module boundaries and [docs/ROADMAP.md](docs/ROADMAP.md) for the staged path to a desktop companion and supervised autonomy.
