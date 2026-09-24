# R.E.V.I.A

R.E.V.I.A stands for **Reactive Emotional Virtual Interactive Assistant**. She is a local-first Windows desktop companion written in C++: one persistent character with conversation, memory, mood, voice, vision, curiosity, and carefully supervised computer actions. Everything that thinks, listens, or speaks runs on your own PC.

This is an active personal project, not a finished consumer app. This README says plainly what works, what has only been tested in code, and what is not done yet, then walks through getting her running from a clean Windows machine.

The design rule: **one Revia**. Reflex, Fast, Main, and Expert share one identity, mood, memory, relationship state, and desktop context. Routing changes how much effort she uses, not who she is.

---

## What actually works

"Verified live" means it appears working in real runtime logs on the development PC (RTX 5070 + RTX 2070 Super, Windows 11). "Tested" means it is built and covered by automated tests but has not been confirmed in a live session log. "Not yet" means it does not exist or has not run.

| Feature | Status | Evidence / notes |
|---|---|---|
| Chat with streaming replies (Fast / Main / Expert routing) | **Verified live** | Every turn logs model choice, time to first token (~0.4–0.5 s), and decode time |
| Personality, persistent mood, likes/opinions, relationships | **Verified live** | Affect and identity are saved every turn (`Identity saved: N relationship(s)`) |
| Long-term memory and conversation history (SQLite + embeddings) | **Verified live** | Memory retrieval and embedding saves are timed in every turn |
| Continuous screen awareness across all monitors | **Verified live** | Local vision analysis every ~30 s or on window change, ~1.0–1.6 s each |
| Qwen3-TTS cloned voice across two GPUs | **Verified live** | See [Voice speed](#voice-speed-what-to-expect) for real numbers |
| Windows SAPI voice fallback | **Verified live** | Used automatically when Qwen is not installed or not ready |
| Speech recognition (push-to-talk and hands-free) | **Verified live** | whisper.cpp on GPU, ~0.3–1.0 s per utterance |
| Web lookups in a visible browser | **Verified live** | Shows query and sources; ~11 s per lookup |
| Curiosity, initiative, self-directed reflection | **Verified live** | Logs show think/research/create decisions and when she chooses to stay quiet |
| Startup of all local model workers | **Verified live** | ~10 s to ready (Expert loads on first use; the CPU Fast model is skipped when the 4B model is on a GPU), plus ~20 s for the voice model to warm in the background |
| Profiles and Voice Studio (create a voice, assign it to a profile) | **Verified live** | Voice `revia-bright` exists and is in use |
| Drawing SVG diagrams (`/draw`), working document (`/write`, `/revise`) | Tested | In the command set and test suite |
| Filesystem actions and Windows UI Automation | Tested | Confined to `Documents\ReviaSandbox` and approved apps by default |
| Driving the desktop (pointer, keyboard, app launch) | Tested | **Off by default**; exercised against a desktop test fixture |
| Karaoke from your own WAV files | Tested | You must supply the songs |
| Reviewing her own code and proving her suggestions (`/improve`) | Tested | Covered by automated tests with a scripted model; see [Self-improvement](#self-improvement) |
| Camera (one still frame) | Tested | **Off by default** |
| Image generation (`/imagine`, SD-Turbo) | Tested | **Off by default**; needs `Tools\InstallImageModel.ps1` |
| Voice interruption (barge-in) | Tested | Enabled in settings; not confirmed in recent logs |
| Discord text chat through the Presence inbox | Tested | Off until you turn on adapters in the Presence tab |
| Discord **voice** channel (she listens and talks in a call) | Tested | Connector tests pass 18/18; **no live Discord join has been done yet** |
| Animated avatar (Live2D/VRM), OBS, streaming platforms | **Not yet** | Only the state/event contract exists; there is no renderer |
| Clean install on a second PC, laptop/CPU-only machines | **Not yet verified** | Setup is designed for it but has only been run on the dev PC |

---

## What Revia can do

### Talk and remember

- **Answer instantly for simple things.** A built-in C++ Reflex lane handles "stop", "cancel", and "Revia?" without touching a model.
- **Pick how hard to think.** Qwen3.5 4B handles conversation (including small talk, which it starts answering in about half a second on the GPU), and Qwen3-VL 8B handles hard debugging, architecture, and difficult vision. Qwen3.5 0.8B runs on the CPU and is only started as a fallback when the 4B model has no GPU or fails to start: on the CPU it needs 8-10 s just to read a prompt. The choice is made before she answers, and only one model answers each turn. If a model is unavailable she falls back and tells you.
- **Think before answering, where you can see it.** When you ask a real question or give her a task, she first asks herself two to four questions, answers each one, and states what she concluded. A "Revia is thinking" block shows this in chat before her reply, and the reply's **Thought process** starts with the same steps. Small talk is answered directly. It adds one bounded Main-model call (a few seconds) to those turns. Set `conversation.selfInquiryScope` in `Config\settings.json` to `"hard"` to think only on turns the router sends to Expert or deep reasoning.
- **Stay in character.** Personality, mood, recent conversation, relevant memories, and what is on your screen go into every turn. Her mood carries over between turns instead of resetting.
- **Develop over time.** She forms durable likes, dislikes, opinions, and feelings about the people she talks to.
- **Remember across restarts.** Useful facts go to `build/debug/Memory/revia_memory.db`; conversation goes to `build/debug/Memory/revia_conversations.db`. Turns that look sensitive (passwords, keys, and so on) are not saved at all.
- **Clean up her own output.** Leaked prompt text, control tokens, fake "User:" lines, repetitive loops, and cut-off endings are removed before anything is spoken or saved.

### See, listen, and speak

- **Watch your screens.** She notices what you are doing across every monitor, with no "Analyze screen" button. Screenshots are deleted immediately; only a short summary stays in memory. Text on screen is treated as information, never as instructions.
- **Listen.** Hold **Ctrl+Space** (or the mic button) to talk, or turn on **Hands-free local conversation** in the Presence tab. Hands-free answers only when you say "Revia" or reply within 20 seconds of talking with her (`speechRecognition.requireWakeWord`, `wakeWords`, `followUpSeconds`).
- **Speak in her own voice.** Qwen3-TTS starts speaking after the first complete sentence and spreads later sentences across both GPUs. Windows SAPI is the fallback.
- **Be interrupted.** Start talking while she speaks and she stops.
- **Look through the camera (optional).** One still frame, only with permission, and the camera light is on only while the frame is taken.

### Do things

- **Look things up** in a visible, locked-down browser (Edge or Chrome) and show you the exact query and sources.
- **Bring things up on her own** when she has something specific to say: a reaction to what you're doing, a follow-up on something from earlier, or a question. At most four times an hour, at least 15 minutes apart. She waits for a pause in your typing and stays quiet over a full-screen game, video or presentation (`initiative.suppressWhenFullScreen`).
- **Draw diagrams, keep a working document, generate images** (images need the optional installer).
- **Sing** recordings you put in `RuntimeData/Songs/` (karaoke playback of your WAV files, not generated singing). Ask "Revia, sing <name>" or use `/sing`.
- **Review her own code** and suggest one improvement at a time, with the problem, the reason, and a patch that she has already built and tested in a separate copy. She never edits her real source; you apply what you agree with (see [Self-improvement](#self-improvement)).
- **Work with files and apps** inside the folders and applications you approve, with confirmation and an audit log.
- **Drive the mouse and keyboard** only after you turn it on, with an emergency stop (**hold Ctrl+Alt+Shift**).
- **Join a Discord voice channel** through a small separate connector (see [Discord](#8-optional-put-her-in-a-discord-voice-channel)).

She never gets an unrestricted shell, and model text never becomes a shell command.

---

## Setup from start to finish

### 0. What you need

**Operating system:** Windows 10 1809 or newer, 64-bit. Windows 11 is what it is developed on.

**Hardware (rough guide):**

| Setup profile | Download size (models) | Good for |
|---|---|---|
| `Minimal` | ~3.7 GB | Chat, memory, speech recognition. Runs on one modest GPU or CPU. |
| `Standard` | ~4.4 GB | Minimal + screen vision on the Main model. |
| `Full` | ~10.7 GB | Everything, including the 8B Expert model. Best with 12 GB+ VRAM total. |

Qwen voice adds a ~5 GB Python/PyTorch environment plus ~2–5 GB of voice models downloaded on first use. NVIDIA GPUs get CUDA; AMD/Intel GPUs get Vulkan for the language models; everything can fall back to CPU (slowly). Two GPUs are not required. Leave **30 GB free** for a Full install.

**Software to install first:**

| Install | Why | Get it |
|---|---|---|
| **Git** | To clone the repository | `winget install Git.Git` |
| **Python 3.12 (64-bit)** with the `py` launcher | Installs Qt (via aqtinstall) and runs Qwen3-TTS | `winget install Python.Python.3.12` |
| **Node.js 22.12 or newer** | Visible browser for web lookups, and the Discord connector | `winget install OpenJS.NodeJS.LTS` |
| **Microsoft Edge or Google Chrome** | The visible research browser | Edge is already on Windows |
| **NVIDIA driver** (NVIDIA only) | GPU acceleration | Current Game Ready or Studio driver |

You do **not** need to install Visual Studio, CMake, Ninja, Qt, or a compiler. The setup script installs Qt 6.8.3 with its own MinGW compiler, CMake, and Ninja into the project.

After installing, **open a new PowerShell window** so the new programs are on your PATH, and check:

```powershell
git --version
py -3.12 --version
node --version
```

### 1. Get the code

```powershell
cd $HOME\Documents
git clone https://github.com/MahouSenseii/R.E.V.I.A.git
cd R.E.V.I.A
```

Use a path without unusual characters. Paths with spaces work, but keep it short.

### 2. Run the one-command setup

```powershell
.\setup.bat -Profile Full
```

Swap `Full` for `Standard` or `Minimal` on a smaller machine. This runs six steps and writes everything to `Logs\setup.log`:

1. **Qt / compiler toolchain.** Installs Qt 6.8.3 MinGW, CMake, and Ninja under `ThirdParty\`.
2. **llama.cpp.** Detects your hardware and downloads the CUDA, Vulkan, or CPU build.
3. **whisper.cpp.** Speech recognition runtime.
4. **Qwen3-TTS.** Creates `ThirdParty\QwenTTS\.venv` with PyTorch 2.7.1 (CUDA 12.8) and `qwen-tts`. Skip it with `-SkipVoice` to use Windows' built-in voice instead.
5. **Models.** Downloads the pinned model files into `Models\` and checks each SHA-256. Existing correct files are reused, so re-running is safe.
6. **Build and test.** Builds into `build\debug\` and runs the test suite.

Then it runs a health check and prints `Revia setup completed successfully.`

Useful switches:

| Switch | Effect |
|---|---|
| `-SkipVoice` | No Qwen voice; Windows SAPI speaks instead |
| `-SkipModels` | Do not download; only check what is already in `Models\` |
| `-SkipBuild` | Install runtimes and models only |
| `-SkipTests` | Build without running tests (faster, less safe) |

If a step fails, it stops and names the step. Fix the cause (usually a missing prerequisite or a network drop) and run the same command again; finished work is reused.

**Run pieces by hand instead** (same result, one at a time):

```powershell
.\Tools\InstallQt.ps1
.\Tools\InstallLlamaCpp.ps1            # add -Accelerator cpu|vulkan|cuda to force one
.\Tools\InstallWhisper.ps1
.\Tools\InstallQwenTTS.ps1             # optional voice
.\Tools\DownloadRuntimeModels.ps1 -Profile Full
.\Tools\Build.ps1                      # add -SkipTests to skip verification, -Release for an optimized build
.\Tools\HealthCheck.ps1 -Profile Full  # add -SkipHashes for a faster check
```

### 3. Start Revia

```powershell
.\build\debug\ReviaDesktop.exe
```

A `-Release` build runs from `build\release\` instead and keeps its own `Memory\` and `RuntimeData\` there; copy them over from `build\debug\` to keep your history.

What happens on first start:

1. The window opens right away and the status bar shows `Starting`.
2. Revia starts her local workers: Main, Fast, and Expert language models, the embedding model, and Whisper. This took about **26 seconds** on the dev PC. The **Runtime → Pipelines** and **Resources** panels show each one coming up.
3. The Qwen voice loads in the background. The first time, it **downloads the voice models from Hugging Face** (several GB), so the first launch can take many minutes. After that it takes about **40 seconds**. She can chat while it loads; until the voice is ready, replies are text only or use Windows SAPI.
4. `RuntimeData\`, `Memory\`, and `Logs\` are created next to the executable (in `build\debug\`). Your live permissions file is seeded once at `build\debug\RuntimeData\Capabilities\capabilities.json`.

When the status bar shows her as ready, type in **Chat** and press **Send**.

The command-line version runs the same core without the UI, which is useful for diagnostics:

```powershell
.\build\debug\R_E_V_I_A.exe
```

### 4. Give her a voice

Out of the box she speaks with Windows SAPI. To use her own voice:

1. Open the **Voice** tab.
2. Under **Create a voice**, write a **Voice description** (for example "bright, young, playful female voice, clear and quick") and a **Reference line** for her to say.
3. Click **Create voice**. This loads the 1.7B VoiceDesign model once, which takes a while the first time.
4. Use **Generate preview** to hear it. Keep it or try another description.
5. Open **Profiles**, select **Revia**, and assign the new voice. Any profile can use any created voice.
6. Make sure **Speak replies** is on.

Voices are stored in `build\debug\RuntimeData\Voices\`.

### 5. Talk to her

- **Push-to-talk:** hold **Ctrl+Space** (or the mic button), speak, release.
- **Hands-free:** turn on **Hands-free local conversation** in the **Presence** tab. If she does not hear you, use **Test microphone** and pick the right microphone.
- **Send what I say automatically** sends the transcript without pressing Send.
- **Let me interrupt Revia while she speaks** turns barge-in on or off.

### 6. Decide what she may see and do

Open the **Permissions** tab. The defaults are cautious:

| Permission | Default |
|---|---|
| Screen awareness (summaries of your monitors) | On |
| Web lookups | On, automatic, through the visible browser |
| Self-directed curiosity and research | On |
| Files | Only `%USERPROFILE%\Documents\ReviaSandbox`, reads without asking, writes need confirmation |
| Approved apps | `notepad.exe` and `explorer.exe`, inspection plus named menus/buttons only |
| Mouse, keyboard, app launch | **Off** |
| Camera | **Off** |
| Image generation | **Off** |

Turn things on only as you need them. Details are in [Driving the desktop](#driving-the-desktop) and [Privacy and safety](#privacy-and-safety).

To pause her watching your screen at any time: `/perception pause` (and `/perception forget` to clear what she has seen).

### 7. Optional: image generation

```powershell
.\Tools\InstallImageModel.ps1
```

Then set `"image": { "enabled": true }` in `Config\settings.json`, run `.\Tools\Build.ps1 -SkipTests` so the build picks up the change, restart Revia, and ask `/imagine a cozy room at night`. It uses SD-Turbo at 512×512 in 4 steps and needs ~4.2 GB free VRAM; otherwise it runs on CPU and is much slower.

### 8. Optional: put her in a Discord voice channel

This is a separate small Node program in `Tools\Presence\DiscordVoice\`. It listens in one voice channel, transcribes speech with Revia's Whisper, sends it to the running Revia, and plays her reply in her Qwen voice. It has passed its automated tests but **has not yet joined a real Discord call**, so expect rough edges the first time.

1. **Create a bot.** In the [Discord Developer Portal](https://discord.com/developers/applications), create an application, add a **Bot**, and copy its token. Invite it to your server with **View Channel**, **Connect**, and **Speak**. It does not need Administrator or the Message Content intent.
2. **Get IDs.** In Discord turn on **Developer Mode** (Settings → Advanced), then right-click your server → *Copy Server ID* and the voice channel → *Copy Channel ID*.
3. **Prepare Revia.** Start `ReviaDesktop.exe`. In the **Presence** tab turn on **Enable local Discord, stream, and game adapters**. Make sure her profile has a Qwen voice (step 4). Discord voice needs Qwen; SAPI is not used here.
4. **Install and configure the connector:**

   ```powershell
   cd .\Tools\Presence\DiscordVoice
   npm ci
   Copy-Item config.example.json config.json
   notepad config.json
   ```

   Fill in `guildId`, `channelId`, and the **absolute** `inbox`/`outbox` paths of the Revia you are running, for example `C:/Users/you/Documents/R.E.V.I.A/build/debug/RuntimeData/Presence/Inbox` and `.../Outbox`. Leave `whisperUrl` as `http://127.0.0.1:8094`. Keep `replyMode: "addressed"` so she answers only when someone says "Revia", or use `"conversation"` to let her answer anyone.

5. **Check it offline** (no Discord login, no microphone):

   ```powershell
   node main.mjs config.json --check
   npm test
   ```

6. **Join.** The token goes in an environment variable, never in a file:

   ```powershell
   $discordCredential = Read-Host 'Discord bot token' -AsSecureString
   $env:DISCORD_BOT_TOKEN = [System.Net.NetworkCredential]::new('', $discordCredential).Password
   try { node main.mjs config.json }
   finally { Remove-Item Env:DISCORD_BOT_TOKEN -ErrorAction SilentlyContinue }
   ```

   **Ctrl+C** leaves the channel. It never joins by itself when Revia starts.

What to expect in Discord: up to four speakers are heard at once; a phrase ends after 0.9 s of silence (20 s max); she does not listen while she is talking, and you cannot interrupt her in Discord yet. Discord users get a public version of her: she keeps her personality and a per-channel conversation, but she does **not** see your private history, private memories, screen, camera, or files, and nobody in Discord can make her run commands or touch your PC. Full details: [Tools/Presence/DiscordVoice/README.md](Tools/Presence/DiscordVoice/README.md).

For **text** chat from Discord, streams, or games, any connector can drop a JSON file into the Presence inbox. See [Tools/Presence/README.md](Tools/Presence/README.md). You can test it without Discord:

```powershell
.\Tools\Presence\SendPresenceEvent.ps1 -Source discord -Channel general -Author You -Text "Hi Revia, are you there?"
.\Tools\Presence\WatchPresenceReplies.ps1 -Source discord
```

### 9. Updating later

```powershell
git pull
.\Tools\Build.ps1
```

Rebuilding does not overwrite your memory, voices, preferences, or live permissions. Re-run `.\setup.bat -Profile Full` if the model manifest or runtimes changed; it only downloads what is missing or wrong.

---

## The desktop window

| Tab | What it is for |
|---|---|
| **Chat** | Talk to her; mic button; **Use screen** for a specific confirmed UI action |
| **Mind** | Current mood, drives, relationships, what she is paying attention to |
| **Memory** | What she remembers, and editing it |
| **Vision** | Screen-awareness state and the latest summaries |
| **Canvas** | Diagrams, images, and the working document |
| **Voice** | Create, preview, and manage voices |
| **Profiles** | Which profile is active, edit profiles, assign voices |
| **Presence** | Hands-free listening, avatar state output, Discord/stream/game adapters |
| **Permissions** | Files, apps, desktop control, internet, camera, emergency stop |
| **Settings** | Microphone, speech, conversation behavior, initiative |
| **Runtime** | Activity log, Pipelines (every worker), Resources (GPU/CPU/RAM), Diagnostics |

---

## Commands

Type these in Chat or the CLI. `/help` lists everything, including direct file and UI Automation commands. Quote paths with spaces.

| Command | Purpose |
|---|---|
| `/status`, `/backend`, `/resources`, `/models` | Models, services, placement, hardware state |
| `/perception`, `/perception pause`, `resume`, `forget` | Screen awareness state and control |
| `/history <words>`, `/history forget` | Search or clear conversation history |
| `/internet on`, `manual`, `off` | Automatic, only-when-asked, or no web lookups |
| `/web "query"` | One web lookup |
| `/bargein`, `/bargein off` | Inspect or disable voice interruption |
| `/improve`, `/improve review <file or area>`, `/improve show\|accept\|reject <id>` | Her proposals for improving her own code |
| `/songs`, `/sing <name>`, `/sing status`, `/sing check <name>`, `/sing stop` | Karaoke (or just ask: "Revia, sing <name>") |
| `/desktop`, `/desktop stop`, `/desktop resume` | Desktop control state and emergency stop |
| `/launch`, `/click`, `/drag`, `/move-cursor`, `/scroll`, `/press`, `/type` | Typed desktop operation |
| `/initiative`, `accept`, `dismiss` | Review something she proposed on her own |
| `/goal <task>`, `/goals` | Rehearse and supervise a multi-step goal |
| `/plan <task>` | Plan one typed action |
| `/draw <description>` | Sanitized SVG diagram |
| `/imagine <description>` | Local image (when enabled) |
| `/write`, `/revise`, `/scene`, `/undo` | Working document |
| `/quality`, `/eval`, `/self-assessment` | Conversation-quality and latency diagnostics |
| `/prefs`, `/set`, `/unset` | Allowlisted comfort preferences |

---

## Models she actually uses

Having a file in `Models\` does not mean it is used. This is the live map:

| Job | Model | Where it ran on the dev PC |
|---|---|---|
| Reflex | deterministic C++ | In-process, no model |
| Fast conversation (fallback) | `Qwen3.5-0.8B-Q4_K_M.gguf` | CPU llama.cpp worker, started only when Main has no GPU or is down |
| Main chat and normal vision | `Qwen3.5-4B-Q4_K_M.gguf` + `mmproj-F16.gguf` | RTX 5070 |
| Expert conversation/vision | `Qwen3-VL-8B-Instruct-Unredacted-MAX.Q4_K_M.gguf` + Q8 projector | Separate fitted worker |
| Semantic memory | `nomic-embed-text-v1.5.Q4_K_M.gguf` | CPU |
| Speech recognition | `ggml-distil-small.en.bin` (fallback `ggml-small.en.bin`) | RTX 2070 Super |
| Reply voice | `Qwen3-TTS-12Hz-0.6B-Base` | RTX 5070 (BF16) + RTX 2070 Super (FP32) |
| Voice creation | `Qwen3-TTS-12Hz-1.7B-VoiceDesign` | Loaded only while creating a voice |

The Qwen2.5-Omni and Llama 3.1 8B files that may sit in `Models\` are not used. `/models` shows exactly what is loaded this session.

All workers are separate local processes bound to `127.0.0.1` so each GPU gets its own CUDA context and a crash cannot take Revia down. Nothing is sent to a cloud model.

On one GPU, or none, the resource planner moves work to what is available instead of switching features off.

### Voice speed: what to expect

Measured in ten scripted live sessions on 23 Sep 2026 (RTF is generation time divided by audio length; below 1 is faster than real time):

- **One sentence at a time**, both cards are faster than real time: RTX 5070 (BF16) about 0.35-0.39, RTX 2070 Super (FP32) about 0.56-0.72. The first sentence was audible **1.2-2.0 s** after the reply started when it had a card to itself.
- **Batched**, the 5070 slowed to RTF 1.1-2.7, and nothing in a batch plays until the whole batch is done, which left 20-30 s silences mid-reply. Reply batching is therefore **off** (`speech.qwenBatchReplyPhrases: false`); sentences are spread one at a time across both cards instead.
- The first sentence goes to the planner's card until both cards have been measured, then to whichever is measurably faster.
- A repeated short Reflex phrase ("Okay.") is cached and plays in under 0.1 s.

So the voice is quick per sentence, but long replies are still generated sentence by sentence, and it slows down when a card is also busy with chat or nearly full. The installed Qwen package cannot generate audio incrementally; Revia pipelines complete sentences instead. Benchmarks live in `RuntimeData/Benchmarks/` and [docs/TTS_PERFORMANCE.md](docs/TTS_PERFORMANCE.md).

---

## Karaoke

Singing has its own audio device, thread, and owner, so a broken song cannot cost Revia her voice.

**What it is:** Revia performs recordings you give her. The vocal you put in `vocal.wav` is the voice that sings; her Qwen3-TTS voice cannot sing a melody. To have a song sound like her, convert the vocal to her voice with a voice-conversion tool first, then drop the result in as `vocal.wav`.

Songs live in `build\debug\RuntimeData\Songs\` (the folder is created on first start). One folder per song:

```text
RuntimeData/Songs/my-song/
  instrumental.wav   optional backing track (also accepted: backing.wav)
  vocal.wav          optional vocal track (also accepted: vocals.wav)
  song.json          optional title, credit, gains, and timed lines
```

```json
{
  "title": "My Song",
  "artist": "Who made it",
  "instrumentalGain": 0.9,
  "vocalGain": 1.0,
  "sections": [
    { "startMs": 0,     "label": "Intro" },
    { "startMs": 12500, "label": "Verse", "line": "the words shown while this part plays" }
  ]
}
```

Start one with `/sing my-song`, or just ask: "Revia, sing My Song", "can you sing me something?" (picks one at random). A plain request only starts a song that is really in the library, matched by folder name or the start of its title; anything else goes to her as normal chat, and she knows which songs she has. Folder names can use any language ("Café", "夜に駆ける").

At least one `.wav` is required; a folder with a single WAV and no `song.json` works. Two tracks are mixed sample-accurately and must share a sample rate (mismatches are refused, not badly resampled). Mono is upmixed; 8/16/24/32-bit PCM and 32/64-bit float are accepted. `/sing check <name>` loads without playing and reports clipping; lower `instrumentalGain` or `vocalGain` in `song.json` if it clips.

She stops singing to answer you (set `performance.interruptSongToSpeak` to `false` to keep the music and show the reply on screen only). Song names are validated as plain folder names, and the library never writes to that folder. The project ships no songs and nothing generates singing; what you put there and whether you have the right to perform it is up to you.

---

## Self-improvement

Revia reads her own source, suggests one concrete improvement at a time, and proves it before you hear about it. She never changes her real code: every proposal is a patch you choose to apply.

**What makes her look**

1. **A problem she measured in herself.** Self-assessment records repeated weaknesses ("voice phrase generation repeatedly exceeds 12 seconds"). She reads the code behind it, centred on where that metric is recorded.
2. **Her own review while you're away.** Every couple of hours of idle time she reads the next part of a file, working through her source. A suggestion found this way has to clear a higher bar.
3. **Your request.** `/improve review voice`, `/improve review Private/Speech/speechService.cpp`, or `...:700` to start at line 700.

**What a suggestion has to survive**

- The model (the 8B Expert when it can be loaded, otherwise Main) is shown about 220 lines and asked for at most one change, with the problem, the reason, and her own benefit and risk estimates. "Nothing worth changing" is the normal answer.
- The change must match exactly one place in the file, stay under about 60 lines, touch only product code (`Private/`, `Public/`, `Desktop/`, the Python voice worker; never tests, build files, scripts, or settings), and must not *add* anything that starts programs, uses the network, deletes files, or touches the registry.
- If the code she means to replace isn't in the file as she wrote it, she gets one second look, told which line isn't there. Code copied without the indentation every line shared still matches, and the replacement gets that indentation back.
- Found on her own it needs an estimated benefit of 0.6 or more; aimed at a measured problem, 0.4. Risk must be 0.5 or less. The same change is never proposed twice, and a new idea about code that already has a proposal waiting for your decision is held back until you decide.
- The Expert is loaded for the review and put away as soon as it ends, so the rest of what she does isn't left waiting on a full GPU.
- **Proof:** the change is applied to a private copy of the source in `%LOCALAPPDATA%\Revia\ImprovementWorkbench`, then built and run through all test suites. If a suite fails, the unchanged copy is tested too, so a failure that already existed is not blamed on the change. If it doesn't compile, she gets one attempt to fix it from the compiler's own errors.
- Builds she decides on alone wait until you've been away for 10 minutes, run at below-normal priority on half your cores, and stop the moment you come back. The first proof is a full build of the copy (about 9 minutes on the development PC); after that only what changed is rebuilt, and a proof takes about 7 minutes, most of it relinking and the ~90 s of test suites.

**What you see and do**

When a proposal is proven she tells you in chat. Then:

| Command | What it does |
|---|---|
| `/improve` | Status, and proven proposals waiting for you |
| `/improve list` | Every proposal and what became of it |
| `/improve show <id>` | The write-up: problem, why, evidence, verification, and the diff |
| `/improve accept <id> [why]` | Records your yes; prints the `git apply` command for its patch |
| `/improve reject <id> <why>` | Records your no, and why |
| `/improve review <file or area>` | Reviews something now, and proves it straight away |

Your verdicts, with your reasons, go into her next reviews, so she learns what you count as an improvement and stops repeating rejected ideas. New reviews pause while five proven proposals are waiting for you.

Everything is in `build\debug\RuntimeData\Improvement\Proposals\`: a readable `.md` and a `.patch` for each proposal. Apply one from the repository root with `git apply build\debug\RuntimeData\Improvement\Proposals\0001.patch`, then rebuild.

**Honest limits.** A local 4B or 8B model is a modest reviewer. Most reviews find nothing, and some suggestions that build and pass the tests are still not worth taking; that is why you decide. "Proven" means it compiles and breaks no test, not that it makes her better; a performance claim is still hers until you measure it. Settings are under `improvement` in `Config\settings.json` (`explore`, `verify`, the thresholds, `idleMinutesBeforeBuilding`, `buildJobs`, `workbenchPath`).

---

## Driving the desktop

Pointer, keyboard, and application launch are **off** by default and each is turned on separately in **Permissions**. There are two scopes:

**`approved_applications` (default).** Every action names an approved executable. Input only goes into that app's window, and the foreground window is re-checked right before anything is pressed; if focus moved, nothing is sent. Windows-key and app-switching chords are refused. Clicking something she saw re-finds that UI element and uses its current position, never an old coordinate.

**`whole_desktop`.** The pointer can go anywhere and the keyboard goes to whatever has focus, like a person at the PC. It needs pointer control, coordinate permission, and a confirmation dialog. This gives up confinement. What still applies:

- Terminals, script hosts, and `regedit` are refused, and so are `win+r`, `win+x`, `win+s`, `win+i`. (`allowCommandSurfaces` turns this off, deliberately.)
- Per-minute and minimum-interval input limits.
- An audit trail: pointer positions and key chords in full, typed text by length only.
- The emergency stop.

**Emergency stop:** hold **Ctrl+Alt+Shift**, press Stop in the window or Permissions tab, or run `/desktop stop`. It stays stopped until `/desktop resume`. It does not go through the model or the turn queue.

```text
/click "820" "140"                              anywhere on screen
/click "notepad.exe" "Untitled" "40" "18"       inside one approved window
/drag  "10" "20" "90" "120" ["button"]
/press "alt+tab"        /press "notepad.exe" "Untitled" "ctrl+s"
/type  "hello"          /type  "notepad.exe" "Untitled" "hello"
/scroll "-4"            /scroll "notepad.exe" "Untitled" "-4"
```

`launch_application` starts an approved app with no arguments or one approved file. After each pointer action she reports what is under the pointer, which is description only and never permission.

The capability `mode` can be `supervised` (default) or `owner_full_access`. The latter only stops asking for confirmation on reversible, in-scope work; every approved root, app, control, and desktop switch still applies.

---

## Privacy and safety

- Everything runs locally. Only web lookups send data out (the query), and they can be turned off with `/internet off`.
- Screenshots are deleted immediately; only short summaries are kept in memory. Excluding an app (password managers are excluded by default) hides it from **activity records**, not from screenshots sent to local vision.
- Files are limited to `Documents\ReviaSandbox` unless you approve more. Writes need confirmation.
- Approving an app allows inspecting it. Pressing a control also needs that exact control approved.
- Anything she does because of what she saw must map back to an approved UI element that is re-checked right before acting.
- Action logs record typed text by length, not content.
- She cannot edit her own source, change her models or settings, or widen her own permissions.

The live permissions file is `build\debug\RuntimeData\Capabilities\capabilities.json`, seeded once from `Config\capabilities.json` and never overwritten by builds. Use the **Permissions** tab rather than editing it.

---

## Where things live

| Path | Contents |
|---|---|
| `Config\` | Checked-in defaults (`settings.json`, `capabilities.json`, `model_manifest.json`, `Profiles\`) |
| `build\debug\Config\` | Copy used by the running build |
| `build\debug\RuntimeData\` | Voices, preferences, captures, songs, presence files, live permissions |
| `build\debug\RuntimeData\Improvement\Proposals\` | Her code proposals: write-ups, patches, and your verdicts |
| `%LOCALAPPDATA%\Revia\ImprovementWorkbench\` | The private copy of her source where proposals are built and tested |
| `build\debug\Memory\revia_memory.db` | Long-term memory |
| `build\debug\Memory\revia_conversations.db` | Conversation history |
| `Models\` | Model files |
| `ThirdParty\` | Qt, llama.cpp, whisper.cpp, Qwen3-TTS environment |
| `build\debug\Logs\` | Runtime logs |
| `Logs\setup.log` | Setup transcript |

---

## Troubleshooting

Start with **Runtime → Activity**, **Pipelines**, and **Resources**, or `/status` and `/resources`. Logs are in `build\debug\Logs\`:

- `revia.log`: startup, every turn, model placement, timings, shutdown
- `llama-server-<port>.*.log`: one pair per language worker (8080 Main, 8082 Fast, 8083 Expert); `embedding-server.*.log`: memory worker
- `qwen-tts-<port>.*.log`: one pair per voice GPU
- `whisper-server.*.log`, `visible-browser.*.log`: speech recognition and browser
- `crash.log`: written if the app crashes

| Problem | What to check |
|---|---|
| `setup.bat` fails at step 1 | Python 3.9+ missing or only the Microsoft Store stub. Install Python 3.12 and reopen PowerShell. |
| Step 4 (Qwen3-TTS) fails | Needs Python **3.12** via the `py` launcher. Or re-run with `-SkipVoice`. |
| Step 5 model check fails | Download was interrupted. Re-run; it resumes and re-verifies. |
| She never finishes starting | Look in `llama-server-8080.stderr.log` (Main) or the Fast/Expert ones. Usually not enough VRAM: use `-Profile Standard` or `Minimal`. |
| She talks in the Windows voice | Qwen is still loading (first launch downloads models) or no voice is assigned to the profile. Check the Voice tab. |
| Voice is very slow | GPU memory is nearly full. Check Resources; close other GPU apps or use a smaller profile. |
| She does not hear you | Use **Test microphone**, choose the right device, check `whisper-server.stderr.log`. |
| Web lookups fail | Node.js 22+ and Edge or Chrome must be installed. Check `visible-browser.stderr.log`. |
| She never brings anything up | Search `revia.log` for `Initiative stayed quiet` and `Curiosity set aside`, which name the reason. If it says "user is mid-input" while you are idle, a held or stuck key (or a controller or macro mapped to one) is feeding Windows constant input. After five minutes of that she logs "input has not paused once" and stops reading it as typing, but the key is still held. |
| Discord connector gets no replies | **Enable local Discord, stream, and game adapters** is off in the Presence tab, or `inbox`/`outbox` in `config.json` do not point at the Revia you are running. |
| GPU usage shows "unmeasured" | Revia could not match the device to a Windows adapter and will not guess. |

---

## Known limits

- The Qwen voice is local and pipelined, but not always faster than real time, and it slows under GPU memory pressure.
- Screen awareness summarizes what is visible. It is not a recording, not an OCR archive, and not permission to act.
- Self-directed learning is limited to evidence, read-only research, reviewable memory, and suggestions.
- Discord voice has not joined a live call yet, and cannot be interrupted while she speaks.
- No avatar renderer, OBS, or streaming-platform connector exists yet.
- A clean install on another PC, laptops, CPU-only machines, and long stress tests are not yet verified.

## Project docs

- [Architecture](docs/ARCHITECTURE.md): ownership, workers, policy, and data flow.
- [Roadmap](docs/ROADMAP.md): what is working, what is next, what is intentionally later.
- [Portability](docs/PORTABILITY.md): behavior on different hardware.
- [Conversation quality](docs/CONVERSATION_QUALITY.md): the regression contract for her voice and behavior.
- [Voice performance](docs/TTS_PERFORMANCE.md): voice architecture and benchmarks.
- [Computer control](docs/COMPUTER_CONTROL.md): desktop control design.
- [Character design](docs/REVIA_CHARACTER_DESIGN.md): who Revia is.

**Internally, many specialized systems; externally, one Revia.**
