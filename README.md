# R.E.V.I.A

R.E.V.I.A is a modular C++ desktop assistant. Its Qt shell combines local llama.cpp chat and multimodal vision, hybrid structured memory, affect-aware Windows speech, toggled CUDA whisper.cpp speech recognition, bounded opt-in internet grounding, and supervised filesystem or Windows UI Automation actions. Every external action still passes through deterministic policy, confirmation, dispatch, and JSONL audit boundaries.

The runtime is multi-worker rather than a single blocking pipeline: UI, interactive inference, TTS, STT, and background memory each have independent lifecycle and cancellation ownership. Long-running goals and avatar rendering remain later work; neither should require giving the model unrestricted shell or administrator access.

## What works now

- Local llama.cpp chat through its OpenAI-compatible chat-completions endpoint.
- A Qt 6 desktop window with horizontal component status and chat controls plus Chat, Activity, Pipelines, Resources, Permissions, Voice Studio, and Settings tabs, stop control, always-on-top option, and tray controls.
- Automatic cross-pipeline resource planning. Revia inventories every accelerator exposed by its installed llama.cpp backend, assigns chat/vision, voice, speech recognition, and embeddings independently, budgets CPU threads and bounded RAM caches, and reports the resulting compute map in the Resources and Pipelines tabs.
- **Live usage measured against that plan**, so the Resources tab reports what the machine is doing rather than only what was decided at startup: `VRAM CUDA0 5.6 / 6.5 GiB budget, 8.0 GiB installed`, RAM across Revia and every process it started, and CPU worker load as threads actually busy against the caps the plan set. Video memory is the system-wide adapter figure, because the weights live in a worker process; a backend device that cannot be tied to a display adapter reports **unmeasured rather than borrowing another card's number**. Sampling is observation only — a reading never re-places a worker, because a plan that reacts to its own measurements stops being reproducible. `/resources` prints the same reading, and `resources.usageSampleSeconds` tunes or disables it.
- A persistent Permissions tab that lists every approved Windows application and exact mutable control, discovers actionable UI Automation controls from the foreground window without granting them, and supports explicit add/revoke operations. Application approval alone permits inspection only.
- Opt-in bounded internet grounding. Automatic mode deterministically recognizes current/factual knowledge questions; manual mode leaves ordinary questions local and responds only to explicit web requests. Queries can reach only fixed approved DuckDuckGo and Wikipedia HTTPS APIs, with time, byte, source-count, and rolling request limits. Revia receives sourced text rather than a browser or general socket.
- A bounded affect controller for Revia's own response posture (neutral, curious, pleased, concerned, focused, or confused), with hysteresis, decay, reasons, and visible intensity. It does not infer or invent the user's emotions. The posture is **fed into the model** as a system line each turn, so it colours tone and pacing rather than only driving the status chip and the speech rate. It is phrased as Revia's own stance and explicitly forbids her from naming it, describing her feelings, or assuming anything about the user's.
- A collapsible **Thought process** line under every reply *and every command*. Commands used to leave it blank, which read as "nothing happened" at exactly the moment something did; now a drawing reports the prompt it sent, how much the model returned and how fast, the safety verdict, and where the file went, while any other command names itself and says that it ran as deterministic code with no model call. For a conversation turn it carries the posture that shaped the turn, any reasoning the model emitted in `<think>` tags, whether the reply was spoken in fragments, and where the time went. Reasoning is stripped from the reply itself, so it is never read aloud and never shown inline as if it were an answer.
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
- Bounded multi-step goals with `/goal <task>`: the model plans act/verify steps, the plan is **rehearsed against a disposable copy first**, and only a plan that verified there is offered for approval — with that result attached, so approval rests on an observed outcome rather than on plausible-looking text. Each step must then prove it happened before the next begins, and action/retry/time budgets stop a run that is not converging. `/goals` lists them and `/goals resume <id>` continues one an earlier process left unfinished. Both surfaces submit through the same `ReviaSession`, so the CLI has these commands too.
- Reviewed learning with `/review`: Revia draws conclusions from what actually happened — goal outcomes and whether her unprompted proposals were accepted or dismissed — and offers them with the evidence attached. **Nothing is remembered until you approve it**, and approving one writes an ordinary preference memory. A lesson can never change a capability, a budget, or a policy; the single automatic adjustment in the system (halving the initiative rate below a precision floor) is computed from counted outcomes, not inferred here. Patterns need at least four finished goals or judged proposals, unfinished goals are not counted as outcomes, and the review will conclude that speaking first is unwelcome when that is what the numbers say.
- A conversation-contract evaluation corpus with `/eval`: the regression conversations in `docs/CONVERSATION_QUALITY.md` are run against the active local model and each delivered reply is scored against the clause it exists to defend, with the run appended to `RuntimeData/Evaluations/` as JSONL. An evaluation turn changes nothing — no dialogue history, no durable memory, no posture shift, no speech — and its synthetic turns are quoted beside the live quality counters rather than mixed into them. **A pass carried by deterministic repair is reported as a warning**, because the model's unrepaired reply is recorded and counted alongside the delivered one; an unreachable or stopped model produces unjudged cases rather than false failures. It detects known-bad replies and cannot certify a good one, and says so in its own summary.
- **Durable conversation history.** Every turn is written to `Memory/revia_conversations.db`, searchable with `/history <words>`, and the tail of the previous conversation is replayed into context at startup so a restart continues rather than restarts. This is separate from the fact memory above on purpose: that keeps what a classifier judged worth remembering, this keeps what was actually said, which is a materially larger promise. So it carries its own ceilings (200 conversations, 500 turns each), its own counters, and `/history forget`, which clears the file *and* the live context and vacuums the pages rather than leaving the text recoverable in free space. Turns matching the shared sensitive-content markers are **withheld, not redacted** — a redacted turn still tells you how long the secret was — and the count of what was withheld is reported so the filter's cost is visible rather than inferred.
- **Saved preferences** with `/prefs`, `/set <key> <value>`, and `/unset <key>`, persisted in `RuntimeData/Preferences/preferences.json` and overlaid after `settings.json` is parsed and validated, so a stored value can never bypass validation. The load-bearing property is what it **cannot** write: approved roots, applications, control scopes, execution mode, risk ceilings, internet access, screen capture, and perception are all absent from the writable table and refused by name with an explanation. The table is a fixed allowlist compiled into the binary, so the set of reachable settings cannot grow by accident or by a model inventing a plausible key. A preference command that could widen authority would be an authority escalation wearing a convenient interface.
- **Diagrams and interface mockups.** Ask in conversation — "draw me a diagram of the turn path", "mock up the settings screen" — and it lands on the Canvas tab; `/draw <description>` does the same explicitly. Whether a message is a drawing request is decided by `DrawingRequestPolicy`, deterministic code in the same shape as the internet-lookup recognizer, so it can be read, tested, and corrected rather than re-prompted. It is deliberately specific: "the chart showed a drop last quarter" is a sentence about data, not a request to draw one.

  The model returns **raw SVG rather than JSON**. Escaping a whole document into a JSON string spends most of a small local model's budget on backslashes and fails completely on the first one it gets wrong, and a half-escaped diagram is indistinguishable from no diagram. The sanitizer lifts the element out of whatever prose or fence surrounds it, so the structure that mattered was never the JSON.

  Script, event handlers, external or `file:` references, `foreignObject`, and entity/doctype declarations are **refused rather than stripped** — the rest of a document that carried a `<script>` was written by the same hand — and the refusal says exactly what it found. Qt's renderer does not execute script, but "the current renderer happens not to" is a version, not a security property, so the sanitizer enforces it directly.
- **Generated pictures** with `/imagine <description>`, through a local diffusion model in an owned Python worker. **Off by default** and separate from `/draw` on purpose: a language model emitting SVG draws boxes, arrows, and layouts and cannot draw a scene, while an image model draws a scene and cannot lay out an interface. Collapsing them into one command would guarantee that one of the two is always the wrong tool with no way to ask for the other. `Tools/InstallImageModel.ps1` creates the runtime; the model loads lazily on the first request, so a machine that never asks for a picture pays nothing, and the worker measures free VRAM before choosing a device rather than assuming one — on a single-GPU machine where chat already holds the card it picks CPU, which is slow but finishes, instead of thrashing. Loopback only, per-run bearer key, kill-on-close job, and it refuses to bind anything but localhost.
- **A working document** with `/write`, `/revise`, `/scene`, and `/undo`. Revia drafts into it using the existing material as context for voice and continuity, and then edits it *precisely*: `/revise 4 make her angrier` rewrites line 4 and provably nothing else.

  That guarantee is structural rather than requested. Asking a model to rewrite a scene while preserving everything but one line works until the day it does not, and the failure is silent — the scene still reads fine and a paragraph three pages up has quietly changed. Here the edit path is `ReplaceBlock`, which reaches exactly one block and has no expression for touching another, so the bad outcome is unreachable rather than unlikely. The model is shown two lines either side for continuity and asked for the one line; whatever comes back can only ever be stored in that one block. The single failure the block model cannot prevent by itself — a model that returns the whole scene when asked for a line — is caught by a guard that refuses a replacement containing neighbouring blocks verbatim, and changes nothing. Every mutation snapshots first, so `/undo` steps back through fifty revisions.
- **Pictures on the canvas** with `/show <path>`. Displaying a file is reading it, so it is bounded by the same approved roots that govern reading one, plus Revia's own output folders. `/canvas` lists what is there. Drawings offer a save-a-copy button; a picture already belongs to you, so it does not.
- **Every session records why it ended.** `logs/session-exits.log` gets one `SESSION STARTED` and one `SESSION ENDED` line per run, naming the cause: a typed exit command, Quit from the tray, the window closing with no tray to fall back on, Windows logging out, a startup failure, or a crash. Crashes additionally write `logs/crash.log` and a minidump, and Qt's own fatal and critical messages — which otherwise go to a debugger nobody is attached to — are routed into the same file.

  The cases that matter most cannot report themselves: `TerminateProcess`, a power cut, and some faults leave no opportunity to write anything. So a marker file is written when a session opens and removed only once a reason has been recorded. **A marker still present at the next start is itself the evidence** — the new session logs that the previous one ended without explaining itself, with its start time and process id, in both the ledger and `revia.log`. The first reason recorded wins, so a crash is never overwritten by the generic "event loop ended" that follows it. "It closed on its own again" stops being a report and becomes a lookup.
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

`InstallLlamaCpp.ps1` and `InstallWhisper.ps1` detect the accelerator on this machine and install the matching pinned build: CUDA on NVIDIA, Vulkan on AMD and Intel, or a CPU build otherwise. `InstallLlamaCpp.ps1` selects the CUDA 13.3 package on an RTX 50-series machine and CUDA 12.4 on earlier NVIDIA cards; override that choice with `-CudaRuntime 12.4|13.3`. `InstallQwenTTS.ps1` installs PyTorch 2.7.1 with CUDA 12.8 so the same isolated voice environment supports both Blackwell and Turing cards. It is only worth running if you want voice cloning; Windows SAPI is the zero-setup fallback.

The scripts install pinned Windows CUDA builds of llama.cpp and whisper.cpp, Nomic Embed Text v1.5, Whisper `small.en`, the exact Q8 multimodal projector for the configured Qwen3-VL model, and an isolated Python 3.12 Qwen3-TTS runtime. The native/model artifact installers validate pinned SHA-256 values; the speech installer pins its PyTorch and Qwen package versions, while official Qwen model weights use the Hugging Face cache. The main chat GGUF is not downloaded automatically because it is large and model choice is personal; place it at the `modelPath` in `Config/settings.json`.

The executable changes its working directory to its own folder. CMake copies `Config/` beside it after every build, so runtime configuration is read from `build/debug/Config/`. Runtime memories and action audit records are also kept beside the executable.

The llama.cpp server is optional for direct action commands. It is required for chat, vision, and `/plan`. Revia verifies `/health`, the loaded model identity, effective context/slots, and the vision modality before trusting an existing process.

With `autoStartServer` enabled, Revia first checks `/health` and launches the configured `serverExecutable` only when that endpoint is unavailable. Each owned chat and embedding server receives a fresh cryptographically random API key for that run; all health and inference requests use Bearer authentication. Before either process starts, the resource planner calls that exact executable's `--list-devices`, ranks addressable GPUs by capacity, reserves OS/GPU headroom, and creates one immutable placement plan. The default mixed RTX 5070 + RTX 2070 plan is chat/vision on the 5070, Qwen voice and whisper transcription on the 2070, and embeddings plus SQLite retrieval on CPU. Chat stays on one GPU for low latency whenever it fits; only a model that needs the combined capacity receives llama.cpp's compatible `layer` split with a proportional tensor budget. The planner does not enable experimental tensor parallelism just to raise utilization.

The same plan divides usable logical processors between chat, embeddings, and speech recognition; gives llama.cpp a bounded RAM prompt cache backed by memory-mapped model files; and caps SQLite page/mmap caching while preserving the configured OS-memory reserve. `resources.autoPlan`, the reserve/cache ceilings, and the four values under `resources.assignments` in `Config/settings.json` are the control surface. Symbolic assignments are `auto-primary`, `auto-secondary`, and `cpu`; exact llama.cpp IDs such as `CUDA0` and Qwen/whisper IDs such as `cuda:1` are also accepted. `/status`, the Resources tab, the Pipelines Compute column, and startup logs show what was actually selected.

The plan alone only says what was decided, so the Resources tab also reports what is actually happening. A worker samples every `resources.usageSampleSeconds` (2 by default, 0 to turn it off) and each reading is placed next to the budget the plan set aside for it: dedicated video memory per adapter against `total - gpuReserveMiB`, the resident set of Revia and every process it started against `total - minimumFreeRamMiB`, and processor time consumed per second of wall clock against the chat, embedding, speech-recognition, and voice thread caps. Three numbers rather than two, because "5.6 of 6.5" only means something when the installed capacity is visible too.

Two limits are stated rather than papered over. Video memory is the **system-wide** figure for the adapter -- the weights live in a llama.cpp worker process, so a per-process figure would report Revia using almost none of the VRAM it is responsible for, and these are the counters Task Manager reads, so the two agree. And a backend device that cannot be matched to a display adapter reports **unmeasured**, never a substituted or borrowed number: on a two-card machine, crediting one card's usage to the other would look precise and be wrong.

Sampling is observation only. The planner runs once at startup and is never re-run from a reading, because a plan that moves a worker in response to its own measurements stops being reproducible and starts being a feedback loop. `/resources` prints the same reading, including the per-process breakdown, from the CLI or the desktop.

The default `autoTune` mode still selects a conservative chat context and lets llama.cpp fit GPU layers with automatic flash attention. The context tiers range from 4,096 tokens on low-memory systems through 65,536 on systems with at least 24 GB VRAM and 64 GB RAM. Because Revia serializes foreground conversation state and memory inference that shares the chat model, it uses one chat slot instead of dividing the context between unused slots. This does not serialize the independent voice, STT, embedding, perception, or UI workers.

`autoMaxTokens` derives the response allowance from one quarter of the effective context reported by llama.cpp, with a 512-token minimum and `maxTokens` as the ceiling. The default ceiling is 4,096. An explicit profile `maxTokens` override disables that adaptive calculation for that profile. Set `autoTune` to `false` to use the configured `contextSize` and `parallelRequests` manually. A server that was already running keeps its original launch settings, although Revia still reads its effective context. If `shutdownServerOnExit` is enabled, an owned child is attached to a Windows kill-on-close job; an existing server is left alone. Server output is written under `Logs/llama-server.stdout.log` and `Logs/llama-server.stderr.log`.

Revia's timestamped application log is written beside the executable at `Logs/revia.log`. The desktop activity panel receives the same timestamped lifecycle and timing events. Each chat turn records query embedding, memory retrieval, prompt assembly, request preparation, inference queue wait, wait-to-first-token, decoding after the first token, and total turn time. The `slowest` field identifies the longest non-overlapping stage. Startup, shutdown, automatic-memory classification, document embedding, and memory database saves are timed as well.

Qwen3-VL handles chat/vision; it does not create audio. Voice Studio uses the separate official Qwen3-TTS family: the 1.7B VoiceDesign model turns a natural-language voice description into a reference WAV, then the 0.6B Base model clones that reference for previews and replies. On first run Revia creates `RuntimeData/Voices`, `RuntimeData/Vision`, and `RuntimeData/Capabilities`, then seeds the tracked **Revia Bright** reference and assigns it to the `revia` profile. Existing runtime files and profile choices are never overwritten. Presets and profile assignments are stored in `RuntimeData/Voices/voices.json`, not in the checked-in personality profile, so rebuilding does not erase UI choices. The authenticated worker binds only to `127.0.0.1:8092`, loads one speech model at a time, caches extracted clone prompts, and writes diagnostics to `Logs/qwen-tts.stdout.log` and `Logs/qwen-tts.stderr.log`. The first Create voice and Generate preview operations download their model weights from Qwen's official Hugging Face repositories.

The resource plan resolves `speech.qwenDevice` to an exact CUDA index before the worker starts. The worker checks free memory on that specific card, reports its real device name and dtype, uses FP16 on the RTX 2070/Turing, and uses BF16 only when the selected device supports it. The clone model still warms after chat is usable; insufficient device memory selects CPU, and a synthesis failure falls back to Windows SAPI. Revia intentionally does not split one conversational sentence across heterogeneous GPUs: the plain Qwen3-TTS backend owns one resident model and preserving voice/prosody is more important than forcing both cards to be busy. Independent chat, voice, STT, embedding, and background pipelines can still overlap on their assigned resources.

Semantic memory uses a second llama.cpp process on port `8081`, launched with `--embedding --pooling mean`. Its planned device defaults to `none`, keeping the small embedding model on CPU so it cannot contend with interactive CUDA inference. Query and document prefixes are configured separately. If this server or its model is unavailable, chat continues with SQLite FTS retrieval and `/status` reports the degraded state. Embedding output is stored inside `Memory/revia_memory.db`; no separate vector JSON file is used. RAM is treated as a bounded cache and model page store, not as another compute device.

The current agents are specialized tasks, not long-running autonomous workers. The UI remains responsive on its own thread, visible operations run on a cancellable worker, the memory agent has a background queue, and chat plus embedding use separate llama.cpp processes. Interactive conversation is intentionally prioritized instead of competing with memory classification for the same model slot. Long-running goal agents still belong behind a supervisor with cancellation, budgets, health, and the existing action policy.

## Commands

Paths containing spaces must be quoted.

```text
/help
/status
/backend
/capabilities
/resources
/history
/history resource planner
/history forget
/prefs
/set speech.volume 70
/unset speech.volume
/draw the Resources tab layout
/imagine a rain-soaked street at night, neon signs
/write a short scene: two people arguing about a broken kettle
/revise 2 make her sound tired rather than angry
/scene
/undo
/show "C:\Users\USER\Documents\ReviaSandbox\shot.png"
/canvas
/quality
/eval
/eval list
/eval last
/internet
/internet on
/internet manual
/internet off
/web "current llama.cpp release"
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

The editable runtime configuration is `RuntimeData/Capabilities/capabilities.json`. It is seeded from the repository once and is never overwritten by later builds. `Config/capabilities.json` remains the checked-in default template. The safe default is:

- `mode: supervised`
- only `%USERPROFILE%\Documents\ReviaSandbox` is approved
- only `notepad.exe` and `explorer.exe` are approved for typed UI Automation by default
- every approved executable has an explicit `approvedControls` scope; Notepad and Explorer
  ship with narrow names observed by the disposable UI Automation fixtures and no wildcard
- approving an application with an empty control list grants read-only inspection only;
  mutable Invoke/Value actions require an exact approved accessible name or automation id
- mutable desktop actions are capped at 12 per rolling minute with at least 250 ms between
  admissions
- read-only work can run without a prompt
- filesystem writes require confirmation

Use the **Permissions** tab for normal changes. **Inspect foreground app** minimizes Revia, inventories enabled Invoke/Value controls in the application underneath, and changes no permission until selected rows are approved. Removing a control or application takes effect immediately and persists across rebuilds.

Desktop goal rehearsal never attaches to a pre-existing window. Explorer has a disposable
live fixture. Modern Notepad is used only when the launched window contains a single
isolated document tab; if Notepad restores previous tabs under the user's startup setting,
rehearsal refuses it rather than inventorying that session. This does not remove ordinary
supervised Notepad control from the Permissions tab.

Internet access is disabled by default. Enabling it requires an explicit confirmation because the question text may leave the machine. Automatic lookup can be disabled separately. `/internet` reports the mode, `/internet on|manual|off` changes it, and `/web "query"` always requests one lookup when access is enabled. Results are injected as untrusted reference data and Revia is instructed to cite only the returned URLs. The audit log records the action and query length, not the query text.

### Vision-grounded screen actions

Type one specific instruction in the chat box, then choose **Use screen**. Revia hides its
window, captures only the foreground application window once, and asks Qwen3-VL for a
labelled target region. The window crop keeps its real screen-space origin, and the request
is discarded if foreground focus changes while capture is in progress.
The model cannot click that region. `VisionUiaResolver` must match it to an enabled Windows
UI Automation element using both bounding-box overlap and accessible-name agreement. The
match must clear `resolutionConfidence`, beat the next candidate by `ambiguityMargin`, and
carry a UIA runtime id. Otherwise Revia refuses.

The foreground executable is read from Windows rather than model output and is checked
against `approvedApplications` before inference or UIA enumeration. A successful match
must also appear in that executable's `approvedControls` scope. It then goes through the
ordinary capability decision and confirmation dialog. Execution
rechecks the exact runtime id, name, automation id, and control type after confirmation; a
changed or vanished element fails closed. The temporary PNG is deleted before execution,
and the JSONL audit entry records the model target, region, selected element, and component
scores. There is no coordinate-click fallback.

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

**Initiative is enabled in the checked-in local settings at the user's request.** The capability default remains off. Revia may offer an observation unprompted, but the decision to speak is deterministic policy rather than a model's opinion of its own interestingness — the same reason a model does not choose its own capability scope. A proposal must clear a *confidence* threshold, not a relevance one, and is then subject to:

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

Keep roots, executable names, and control scopes narrow. `approved_scope` does not mean unrestricted PC control: actions outside approved roots/apps/controls, above the risk ceiling, or beyond the desktop rate budget remain blocked, no confirmation prompt can override that block, and all outcomes are audited. Control text values are not copied into the audit log; only their length is recorded.

## Planned: other PCs and cameras

Two capabilities are designed but not built. Both are specified in [docs/ROADMAP.md](docs/ROADMAP.md).

**Stage 8 — other PCs.** Four separate capabilities, not one: showing Revia on a second
machine, talking to her from it, seeing its screen, and operating it. Only the last grants
authority over another machine, so they are staged and the first three add none.

The rule the whole stage hangs from is that **authority is local to the machine that owns
the resource**. Revia asking a remote agent to act is a request, not a command: the far
machine evaluates it against its own capability policy, confirms it there, audits it there,
and may refuse. The alternative — a trusted controller commanding obedient agents — turns a
stolen pairing token or a planner bug into control of every paired machine at once. Pairing
is physical (a code matched on both screens), and remote actions are never auto-approved.

**Stage 9 — camera.** On-demand only: a frame is captured when asked, analysed locally by
Qwen3-VL, and deleted, on the same consent terms as screen capture. Ambient camera watching
is deliberately excluded — most webcams light an LED that software cannot suppress, and
single on-demand frames keep that light meaningful, where a continuous feed would leave it
on permanently and destroy its value as a signal.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for module boundaries, [docs/PORTABILITY.md](docs/PORTABILITY.md) for build requirements and low-end machine behaviour, and [docs/ROADMAP.md](docs/ROADMAP.md) for the staged path to a desktop companion and supervised autonomy.
