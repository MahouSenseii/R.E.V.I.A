"""Hand-written descriptions for docs/CODE_MAP.md.

D maps a unit (Public/Private pairs as "<Domain>/<name>", Desktop pairs as "Desktop/<name>",
anything else by its path or a glob) to one line saying what it does. N holds each domain's
narrative. build_code_map.py refuses to write the map while any tracked file lacks a description.
"""
D = {}
N = {}

# ---- part a ----
N["Runtime"] = """The orchestration layer. `ReviaSession` is the one lifecycle owner both the Qt desktop and the
terminal use: it starts and stops every service and child process, admits one foreground
operation at a time, publishes `RuntimeEvent`s to whatever shell is attached, and composes all
the other owners. Its implementation is split across many `session*.cpp` files that all implement
the same class (delegation, speech, settings, resources, editor awareness and so on).
`ConversationRuntime` owns an admitted conversation turn from routing to delivered reply.
The studios (`agentStudioRuntime`, `audienceStudio`, `learningStudio`, `developmentStudio`) are the
session-side halves of the desktop's studio panels. Why it exists: one owner means desktop and
CLI can never drift apart, and every side effect passes through the same admission and audit."""
D.update({
"Runtime/reviaSession": "The session: lifecycle owner of every service, child process and worker; one foreground operation at a time; publishes runtime events; the API every shell calls.",
"Runtime/conversationRuntime": "Runs one admitted conversation turn: routing and tier choice, grounding (memory, recall, internet, arithmetic, files), prompt assembly, tool loop, streamed reply, speech and timing.",
"Runtime/agentStudioRuntime": "Session side of Agent Studio: starts, resumes, advances, retries and cancels multi-agent workflows, persists them, and provides the model provider workers call (including yielding to the conversation).",
"Runtime/sessionDelegation": "Chat delegation: hands one bounded investigation from a conversation to Agent Studio and reports the reviewed outcome back through Revia's own reply.",
"Runtime/audienceStudio": "Session side of Audience Studio: who Revia is talking to, speaker enrollment and forgetting, evidence corrections and display aliases.",
"Runtime/learningStudio": "Snapshot for the Skills & Learning panel: lesson candidates, kept training captures and their corrections.",
"Runtime/developmentStudio": "Session side of Development Studio: bounded, validated proposals to change Revia's own presentation files, with evidence and review.",
"Runtime/qualityFeedback": "Owner judgments of replies: records feedback, keeps the exact exchange for training on request, corrections, withdrawal, live dataset export, and owner memory revisions.",
"Runtime/companion": "Companion registry and paths: each companion's own RuntimeData root, creation, selection and migration of the legacy single-companion layout.",
"Runtime/companionSettings": "Binds a companion's settings and log directory when it is selected.",
"Runtime/conversationSessions": "Conversation history operations: browse, open, continue, rename, delete, regenerate, retry and edit the last exchange.",
"Runtime/documentWorkshop": "Working document and visuals for a turn: compose and revise document blocks, undo, generate or show images, draw diagrams.",
"Runtime/affectController": "The affect state machine: observes input, turns and runtime-confirmed internal events, classifies them and settles over time, so emotion can shape the current reply.",
"Runtime/affectTypes": "Value types for affect: state, snapshot and social context.",
"Runtime/internalStimulus": "Typed internal outcomes (task succeeded, failed, ...) that may produce affect only once the runtime confirms them.",
"Runtime/idleComputerActivity": "Executes an approved idle computer activity (explore or make something in an approved scope).",
"Runtime/investigationChecks": "Executes one bounded investigation check through the action runtime with admission.",
"Runtime/outputChannelPolicy": "Decides whether a reply is spoken on the current output channel, and why.",
"Runtime/outputChannel": "Where a reply is going (enum).",
"Runtime/channelSettings": "Per-channel conversation settings, such as executables Revia may speak for.",
"Runtime/publicContextCache": "Eviction choice for the bounded per-audience public conversation cache.",
"Runtime/responseLatency": "Measures response latency per turn (text, first audio, first playback) with medians and p95.",
"Runtime/retainedCounts": "Counts warnings and errors still retained in a bounded log.",
"Runtime/runtimeDataBootstrap": "First-run layout: creates RuntimeData directories and copies starter assets without overwriting anything the user has.",
"Runtime/runtimeEvents": "RuntimeState, RuntimeEvent and the thread-safe event bus every shell and panel subscribes to.",
"Runtime/runtimeStamp": "Stamp carried by work: companion, session, generation, task, attempt and policy version; used to reject stale work.",
"Runtime/sessionEditorAwareness": "Editor awareness: observes saved source files from the editor bridge and supplies the current saved-file context to turns.",
"Runtime/sessionIdentity": "Issues and invalidates the session's RuntimeStamp so late work from a previous session is refused.",
"Runtime/sessionLocalApi": "Starts the local HTTP API that external programs use, authenticated by the install's token.",
"Runtime/sessionMcp": "Exposes configured MCP servers' tools to chat, journaled like every action.",
"Runtime/sessionOperator": "Wires operator (computer-control) callbacks and enriches desktop observations for it.",
"Runtime/sessionResources": "Resource monitor wiring: live load sampling and the resource status shown to the user.",
"Runtime/sessionResult": "The result of one interaction returned to a shell (text, success, reason...).",
"Runtime/sessionSettings": "Settings dialog backend: owner overlay over settings.json, live appliers, workspace and session-only switches.",
"Runtime/sessionSpeech": "Adapts speech service events to session state, diagnostics, voice health and system cues.",
"Runtime/sessionTaskContracts": "Builds and validates the task contract for each admitted turn and action.",
"Runtime/sessionVideoGeneration": "Starts a local video generation job and publishes its result.",
"Runtime/speakerEnrollmentRuntime": "Session wiring for voice speaker enrollment (start, cancel, capture samples, resolve speakers).",
"Runtime/turnContext": "Immutable TurnContext (what one unit of work may know) and the turn outcome/event types.",
"Runtime/videoPublicationLane": "Serializes publication of generated videos so shutdown can own the lane.",
"Runtime/agentProviderMode": "Which provider agent workers use: demonstration, local, local with tools.",
})

N["Core"] = """Shared foundations: configuration loading (`configManager`, `settingsFileEditor`,
`preferenceStore`), logging and crash/exit accounting, the bounded conversation context, the
`messageRouter` that fronts every model tier, task contracts and evidence references, and the
thin terminal shell (`reviaApp`). Note for pass 2: despite the name, Core is not a bottom layer
today; it includes Runtime, Agents, Speech and others (see the dependency section)."""
D.update({
"Core/atomicFile": "Replaces a file atomically from a checked temporary, so readers never see a half-written file.",
"Core/appSettings": "The composed settings struct: every domain's settings in one value.",
"Core/commandManager": "Slash commands for shells: parses /commands, help and status output, recognizes /cloud questions.",
"Core/commandOutput": "Result type for a handled command.",
"Core/configManager": "Loads settings.json (with the owner overlay) and profiles; saves and lists profiles.",
"Core/conversationContext": "The bounded in-memory conversation: messages, media history, compression summary and token budgeting.",
"Core/conversationMediaReference": "An immutable reference from a message to stored media (accessed only through the attachment store).",
"Core/conversationMessage": "One conversation message, with attached images shared read-only.",
"Core/crashDiagnostics": "Best-effort crash diagnostics for uncaught exceptions, termination and fatal faults.",
"Core/evidenceBundle": "Bundle of evidence references admitted with a task; validation and serialization.",
"Core/evidenceRef": "One evidence reference (locator plus content identity); provenance, not truth.",
"Core/exitReporter": "Session marker and exit-reason ledger, so an unclean previous exit is detected.",
"Core/localApiKey": "Generates a random local API key.",
"Core/localApiToken": "Loads or creates the install's local API bearer token.",
"Core/logSeverity": "Log severity enum.",
"Core/logger": "Revia's logger: log directory, rotation, severities, timing and prompt-breakdown lines, sinks.",
"Core/memoryManager": "Facade over long-term memory for saving, revising, embedding and duplicate checks.",
"Core/messageRouter": "Front door to the model tiers (Main, Fast, Expert, cloud): routes chat, planning, review, vision and memory requests and reports which model served them.",
"Core/preferenceStore": "Persists the allowlisted non-authority user preferences; refuses unknown keys and authority settings.",
"Core/profile": "aiProfile: a companion profile (system prompt, temperature, voice, personality baseline...).",
"Core/questionRelay": "Relays a worker's question to the UI and its answer back, without deadlocking shutdown.",
"Core/reviaApp": "The terminal shell (CLI) over ReviaSession.",
"Core/runtimePath": "Resolves runtime artifacts and writable paths relative to the launch or program directory.",
"Core/settingsFileEditor": "The only writer of the owner settings overlay; validates allowlisted keys and values.",
"Core/settingsFileKeys": "The allowlist of settings.json keys the owner may change from the desktop.",
"Core/speechAttribution": "Detects reported or quoted speech ('she said ...') so quoted words are not taken as the user's own.",
"Core/taskContract": "Task contracts: what a turn, node or action is obliged and allowed to do; validation and serialization.",
"Core/terminalText": "UTF-8 console input and output on Windows.",
"Core/utf8": "UTF-8 validation, sanitizing and safe truncation.",
"main": "Entry point of the terminal executable (R_E_V_I_A); hands off to reviaApp.",
})

N["Agents"] = """The conversation and its helpers. `TurnCoordinator` runs the `ConversationAgent` for a turn and
queues the `MemoryAgent`'s background work afterwards. Around them: input arbitration (merging
typed and spoken input), the response filter and style policy, reply streaming and fragmenting
for speech, the tool loop and tool registry for chat tools, self-inquiry and investigation agents,
the Agent Studio workflow engine (`agentWorkflow`), source-code review and diagnostics, and
grounding helpers (arithmetic, reminders, reply formats). Why: everything that decides what Revia
says, separated from the session that decides whether she may say it."""
D.update({
"Agents/agentDeliverable": "The required sections and evidence of an agent workflow's deliverable, and their validation.",
"Agents/agentToolWorker": "Schema and parsing for one bounded tool action proposed by an Agent Studio worker.",
"Agents/agentWorkflow": "The Agent Studio engine: parent/worker/review nodes, budgets, attempts, artifacts, acceptance and persistence.",
"Agents/answerObligation": "How strictly a reply must answer (balanced, reliable, character-first).",
"Agents/calculationGrounding": "Restricted arithmetic grounding: asks the model for a calculation proposal and verifies it before it reaches the reply.",
"Agents/chatTools": "Chat tool bindings for actions, files and reminders.",
"Agents/compositionRequest": "Recognizes a self-contained composition request (story, poem...) that needs no grounding.",
"Agents/conversationAgent": "Generates the conversation reply for a turn through the router.",
"Agents/conversationQualityMonitor": "Watches delivered replies for known quality failures (stock tails, invented physical life, denials...).",
"Agents/conversationSettings": "Conversation settings (style, review, limits).",
"Agents/conversationStylePolicy": "Turn guidance and reply refinement: brevity, corrections, acknowledgements, answer obligation, streaming rules.",
"Agents/curiosityAgent": "Nominates and parses idle curiosity activities.",
"Agents/delegation": "Delegation mode and the cue in the person's words that offers chat delegation.",
"Agents/inputArbiter": "Merges, debounces and orders typed and spoken input into batches for the next turn.",
"Agents/inputArbiterSettings": "Input arbitration timing settings.",
"Agents/investigation": "Investigation state: questions, hypotheses, checks, findings, budget and outcome.",
"Agents/investigationAgent": "Runs investigation rounds with the model: opening questions, checks, parsing.",
"Agents/memoryAgent": "Background memory worker: evaluates finished turns for memories, embeddings backfill, learned findings, owner revisions; queues and priorities.",
"Agents/reasoningRelay": "Relays streamed model reasoning to an observer in bounded chunks.",
"Agents/reminderIntent": "Detects whether a request asks for a reminder effect.",
"Agents/replyFormat": "Requested reply formats and contracts (JSON, lists, sentence counts) and matching.",
"Agents/replyFragmenter": "Splits a streamed reply into speakable sentence fragments.",
"Agents/replyStreamGate": "Holds back a streamed reply until it is safe to release.",
"Agents/responseFilter": "Hard and AI response filters applied to replies, including streamed prefixes.",
"Agents/responseFilterSettings": "Response filter settings.",
"Agents/responseProvenance": "Classifies whether a reply may express Revia's own opinion.",
"Agents/sandboxTools": "Sandbox tools for workers: workspace projects, grep, read, patch, run tests and Python in a private sandbox.",
"Agents/selfInquiry": "Self-inquiry: when Revia asks herself questions before answering, with limits and transcript.",
"Agents/sourceDiagnostic": "Static diagnostics over a saved source file (likely bugs a native observation can mention).",
"Agents/sourceDiagnosticReply": "Formats a source diagnostic as a reply and recognizes follow-ups.",
"Agents/sourceExplanationPolicy": "Recognizes requests to explain saved source code (which cap sampling).",
"Agents/sourceQuotation": "Resolves exact quotations from saved source for replies.",
"Agents/sourceReview": "Model review of saved source files, with parsing.",
"Agents/toolLoop": "Runs the chat tool loop: model asks for tools, tools execute through admission, results go back, with limits and audit.",
"Agents/toolRegistry": "The registry of chat tools and their permissions, executors and argument validation.",
"Agents/turnCoordinator": "Runs a turn's conversation agent and owns the memory agent's queue.",
})

N["LLM"] = """Model access. `llmService` fronts llama.cpp (`LLamaCPP/`) and the optional cloud provider
(`Cloud/`); `llamaCppService` builds every request (prompt fitting, tools, images, sampling) and
parses streamed replies; `llamaCppServerProcess` launches and supervises `llama-server` children
with a hardware plan; `inferenceScheduler` shares slots between interactive and background work
with preemption. Why: one place knows the wire protocol and the model processes, so the rest of
Revia talks in typed requests."""
D.update({
"LLM/Cloud/cloudChatService": "Optional cloud chat provider: availability, persona, transport, audit, streamed responses.",
"LLM/Cloud/cloudPolicy": "When a turn may go to the cloud, and building a payload with private context stripped.",
"LLM/Cloud/cloudProtocol": "Cloud provider wire formats and the server-sent-event parser.",
"LLM/Cloud/cloudSettings": "Cloud model settings.",
"LLM/Cloud/cloudTransport": "HTTPS transport for cloud requests.",
"LLM/LLamaCPP/cancellableHttpClient": "HTTP client whose in-flight request can be cancelled.",
"LLM/LLamaCPP/llamaCppEmbeddingService": "Client for the dedicated embedding llama-server.",
"LLM/LLamaCPP/llamaCppServerProcess": "Launches and supervises llama-server children: hardware plan, arguments, batch settings, health.",
"LLM/LLamaCPP/llamaCppService": "The llama.cpp client: builds and sends every chat, planning, review, vision and memory request, fits prompts and parses streams.",
"LLM/LLamaCPP/serverKeyProof": "Proves the running llama-server holds the expected API key.",
"LLM/backendTypes": "Backend kind, system status and health result types.",
"LLM/codeReviewPurpose": "Why a code review request is made.",
"LLM/embeddingTypes": "Embedding result type.",
"LLM/endpointSettings": "llama.cpp and embedding endpoint settings.",
"LLM/imagePromptGuidance": "Prompt guidance for uploaded images and sampled video frames.",
"LLM/imageTokenBudget": "Token allowance for attached images.",
"LLM/inferenceScheduler": "Shares llama.cpp slots: interactive first, background preemptible.",
"LLM/llmService": "Facade over the configured backend (llama.cpp or cloud).",
"LLM/privateMemoryAccess": "Whether a request may read private memory.",
"LLM/promptBuilder": "Builds prompt messages with memory blocks and retrieval queries.",
"LLM/providerExchange": "The exact request/response exchange kept for a judged reply.",
"LLM/responseTypes": "Response, timing and prompt-fit report types.",
"LLM/tokenEstimate": "Token estimates and compaction to a token budget.",
"LLM/toolCall": "Tool definition, call, result and round types for the wire.",
"LLM/videoFrameReply": "Schema and parsing for structured replies about sampled video frames.",
})

N["Intelligence"] = """Which model answers, how hard it thinks, and which models stay loaded. The router picks a tier
(Reflex, Fast, Main, Expert, vision) from typed evidence; `modelLifetime` and
`modelResidencyManager` keep models resident or unload them under leases."""
D.update({
"Intelligence/humanizationState": "Humanization state (mood persistence, curiosity, social energy, confidence, talkativeness...) updated from input and outcomes.",
"Intelligence/intelligenceRouter": "Chooses the model tier and reasoning mode for a turn.",
"Intelligence/intelligenceSettings": "Tier and intelligence settings.",
"Intelligence/intelligenceTypes": "Tier, reasoning mode, routing context and decision types.",
"Intelligence/modelLifetime": "Leases and idle sweeps that decide when a managed model may unload.",
"Intelligence/modelResidencyManager": "Tracks which models are loading, ready, failed or unloaded.",
"Intelligence/reflexRouter": "Answers trivial turns instantly without a model (the Reflex tier).",
})

# ---- part b ----
N["Memory"] = """What Revia remembers. `longTermMemory` stores structured facts with vectors in SQLite and
ranks them with BM25 plus cosine similarity (`hybridRanking`); `conversationArchive` keeps every
conversation and its embeddings for recall; `conversationRecall` decides when a turn should look
back. Optional CPU workers rerank results (`memoryReranker`) and detect near-duplicate memories
(`memoryNliWorker`, `semanticDuplicate`). `sensitiveContent` refuses secrets before anything is
stored. Why: durable, attributable, privacy-scoped memory that the conversation can cite."""
D.update({
"Memory/conversationArchive": "SQLite archive of every conversation session and turn, with embeddings, recall search and session management.",
"Memory/conversationRecall": "Decides when a turn should recall earlier conversation, builds the recall request and renders the block.",
"Memory/hybridRanking": "FTS query building, cosine similarity and reciprocal-rank fusion.",
"Memory/longTermMemory": "SQLite store of structured memories with embeddings, scoped loading, search, revisions and duplicate marks.",
"Memory/memoryAttribution": "Whether a memory is about Revia herself and attributable to her.",
"Memory/memoryCorrection": "Proposes corrections to stored memories from what the owner said.",
"Memory/memoryNliWorker": "Client for the optional local CPU entailment (NLI) worker.",
"Memory/memoryReconciliation": "Classifies how a new memory relates to an existing one (duplicate, update, conflict...).",
"Memory/memoryReranker": "Client for the optional local CPU reranker worker.",
"Memory/memoryRevision": "Owner memory revision request, receipt and scope lease.",
"Memory/memoryScope": "Who a memory is about and which audience scope may read it.",
"Memory/memoryTypes": "Memory entry, importance and decision types.",
"Memory/semanticDuplicate": "Semantic duplicate detection between memories using NLI scores.",
"Memory/sensitiveContent": "Detects secrets and sensitive content that must never be stored automatically.",
"Memory/temporalQuery": "Parses time expressions ('last Tuesday') into time windows for recall.",
})

N["Identity"] = """Who Revia is and who she knows. Persistent development (base personality plus learned
offsets), preferences, per-person relationships and the evidence that moves them, and the
canonical state packet given to every model tier. Saved atomically with schema versions."""
D.update({
"Identity/developmentEngine": "Applies evidence from turns to slowly develop Revia's traits within limits.",
"Identity/developmentState": "Trait vectors, baseline from the profile, drift and its description.",
"Identity/identityStore": "Atomic, versioned persistence of identity (development, preferences, relationships).",
"Identity/preferenceEvidence": "Reads evidence about Revia's preferences from work and curiosity outcomes.",
"Identity/preferenceState": "Revia's preferences: direction, strength, reinforcement and pruning.",
"Identity/promptMarkers": "Canonical state-packet section headers, shared by the renderers and by leak detection (Revia must not read them back out).",
"Identity/relationshipEvidence": "Reads conversation signals (names, warmth, teasing) into relationship events.",
"Identity/relationshipRegistry": "Per-person relationships: load, save, apply evidence, corrections, aliases, recognition consent.",
"Identity/relationshipState": "One relationship's state and how events and time change it.",
"Identity/reviaStatePacket": "The canonical state packet (identity, memories, self-knowledge) for every model tier.",
"Identity/socialIdentity": "Audience, speaker and recognition-consent types.",
"Identity/statePacketRenderer": "Renders the state packet and emotion section as prompt text.",
})

N["Emotion"] = """A continuous emotion vector with slow mood underneath. Stimuli confirmed by the runtime are
appraised (relevance, expectedness, control, social importance) into emotions that decay over
time and feed back into the next appraisal. Nothing here feels anything the runtime did not confirm."""
D.update({
"Emotion/appraisalContext": "Context an appraisal uses (relevant memories, relationship, goals).",
"Emotion/appraisalEngine": "Derives appraisal dimensions from a stimulus and context.",
"Emotion/emotionModel": "The emotion model interface and the rule-based default.",
"Emotion/emotionRuntime": "Runs appraisal, settling and mood, and projects emotion into affect.",
"Emotion/emotionTypes": "Emotions, readings and the emotion vector with decay and valence.",
"Emotion/moodController": "Mood dynamics: how emotions move irritability, energy and sociability.",
"Emotion/moodState": "Mood state and its controller.",
"Emotion/stimulus": "A typed stimulus and its source.",
"Emotion/stimulusBuilder": "Builds stimuli from conversation, goals, discoveries and quiet periods.",
})

N["Autonomy"] = """Whether Revia has any reason to act on her own. Drives build up from evidence, an activity
scheduler weighs evidence against cost and the person's presence, and doing nothing is the normal
outcome. A timer alone never produces activity."""
D.update({
"Autonomy/activity": "An autonomous activity, its type and status.",
"Autonomy/activityExecution": "Resolves research topics and idle computer actions for an activity.",
"Autonomy/activityScheduler": "Decides whether to start an activity from evidence, cost and limits.",
"Autonomy/driveState": "Drives (curiosity, boredom, social, unfinished goal, exploration, learning, creativity) and their dynamics.",
})

N["Initiative"] = """When Revia speaks first. Window events produce cues (focus stretch, return to an app, repeated
switching, a saved file); the attention policy applies confidence, input rhythm, full-screen,
cooldown and budget gates; the initiative controller decides and records outcomes."""
D.update({
"Initiative/attentionPolicy": "Gates whether interrupting now is acceptable (input rhythm, full screen, cooldown, budget).",
"Initiative/conversationStarter": "Turns observed events into conversation cues.",
"Initiative/curiosityJournal": "Remembers topics recently considered or researched.",
"Initiative/initiativeController": "Considers proposals, decides to speak, and records responses.",
"Initiative/initiativeSettings": "Initiative settings.",
})

N["Learning"] = """Learning from outcomes, always through review. Lessons are proposed and need approval; judged
replies can be kept as training captures, corrected and exported as datasets for offline training;
self-assessment tracks capability gaps."""
D.update({
"Learning/datasetExport": "Builds deterministic, provenance-bound training datasets (train/validation/test) from reviewed candidates.",
"Learning/learningRecordStore": "Stores lesson candidates, checks and review decisions.",
"Learning/learningReview": "Draws lessons from confirmed outcomes.",
"Learning/liveCapture": "Kept exchanges of judged replies, their corrections and withdrawal.",
"Learning/liveDatasetExport": "Exports corrected live captures as numbered datasets of a lineage.",
"Learning/qualityFeedback": "Owner quality feedback on a reply and the learning candidate it produces.",
"Learning/selfAssessment": "Tracks capability gaps and proposes self-improvement tasks.",
})

N["Evaluation"] = """Measuring answer quality without producing answers: the conversation-contract corpus, the
golden set and answer gate, a model judge, cognition evaluation and campaign manifests. Used by
tests and the `Tools/Quality` live tools."""
D.update({
"Evaluation/answerGate": "Scores golden-set replies and decides pass/fail against a baseline.",
"Evaluation/answerJudge": "A model judge for replies with a verdict schema.",
"Evaluation/calculationVerifier": "Verifies arithmetic in replies.",
"Evaluation/campaignManifest": "Manifests that pin an evaluation campaign's inputs.",
"Evaluation/cognitionEvaluation": "Cognition cases, oracles and model-judged reviews.",
"Evaluation/conversationEvaluation": "The conversation-contract evaluator and its report.",
"Evaluation/goldenSet": "Golden-set cases and their deterministic checks.",
})

N["Improvement"] = """Revia proposing changes to her own code, safely. A source catalog, review jobs that produce code
proposals, a proposal store, and a private workbench mirror where a proposal is built and tested.
Nothing here edits the live checkout or applies its own proposals."""
D.update({
"Improvement/codeProposal": "A proposed code change: parsing, checks, application to a copy, diffs.",
"Improvement/improvementAgent": "Background reviewer that explores source and proposes improvements.",
"Improvement/improvementSettings": "Improvement settings.",
"Improvement/proposalStore": "Stores proposals, decisions and exploration history.",
"Improvement/selfDevelopment": "Stages and validates self-development candidates.",
"Improvement/sourceCatalog": "Locates and reads reviewable source files.",
"Improvement/workbench": "Private mirror of the source where proposals are built and tested.",
})

N["Speech"] = """Voice in and out. Output: `speechService` turns replies into speech through Qwen3-TTS loopback
workers (`qwenTts*`) with Windows SAPI as fallback, keeping sentence order and cancellation;
vocalizations and system cues add non-verbal sounds. Input: `speechRecognitionService` captures
the microphone, detects voice activity and transcribes with a local whisper.cpp server; speaker
recognition and enrollment identify who is talking; the addressee gate decides whether speech was
meant for Revia. `speechCoordinator` decides who may speak when."""
D.update({
"Speech/addresseeGate": "Decides whether heard speech was addressed to Revia (wake words, recent exchange).",
"Speech/bargeInSettings": "Barge-in (interrupting Revia by speaking) settings.",
"Speech/fileTranscription": "Transcribes a WAV file (used for video audio).",
"Speech/orderedSpeechQueue": "Releases synthesized sentences strictly in order.",
"Speech/playbackEnvelope": "Bounded loudness envelope of Revia's own speech WAV, used to animate the mouth during playback.",
"Speech/qwenTtsClient": "Client for one Qwen3-TTS worker: voices, synthesis, vocalizations, cues.",
"Speech/qwenTtsPool": "Pool of Qwen3-TTS workers across devices.",
"Speech/qwenTtsServerProcess": "Launches and supervises a Qwen3-TTS worker process.",
"Speech/recognitionSettings": "Speech recognition settings.",
"Speech/speakerEnrollmentDialogue": "The spoken dialogue that enrolls a new speaker's voice.",
"Speech/speakerRecognition": "Speaker voice templates: enroll, forget, resolve who is speaking.",
"Speech/speechCoordinator": "Decides which speech intent may speak now (owners, priorities, songs).",
"Speech/speechRecognitionService": "Microphone capture, voice activity, whisper transcription and recognition events.",
"Speech/speechService": "Speech output: synthesis, queueing, playback, cancellation, voice health and studio.",
"Speech/speechSettings": "Speech output settings.",
"Speech/systemCue": "Approved system cue kinds and their definitions.",
"Speech/systemCueBank": "Per-voice bank of rendered system cue clips.",
"Speech/transcriptRouting": "Routes a transcript to conversation, command or nothing.",
"Speech/vocalization": "Non-verbal vocalizations in scripts: parsing, shaping, policy and bank.",
"Speech/voiceActivityMonitor": "Energy-based voice activity detection for hands-free listening.",
"Speech/voicePresetStore": "Saved voice presets and their profile assignments.",
"Speech/voiceTypes": "Voice health, selection, preset and studio types.",
"Speech/wavTiming": "WAV duration and playback hold timing.",
"Speech/whisperServerProcess": "Launches and supervises the loopback whisper-server.",
})

N["Vision"] = """Seeing: one-shot screen capture (whole desktop or the pinned foreground window), single camera
frames, and parsing of model-proposed screen targets. Nothing is held open or retained between captures."""
D.update({
"Vision/cameraCaptureService": "Enumerates cameras and captures a single frame.",
"Vision/screenAwarenessAssessment": "Parses the model's bounded screen-awareness assessment.",
"Vision/screenCaptureService": "Captures monitors or the foreground window.",
"Vision/visionActionParser": "Parses a model's proposed screen target into a typed intent.",
"Vision/visionActionTypes": "Screen region, vision intent and UIA candidate types.",
"Vision/visionSettings": "Vision settings.",
})

N["Perception"] = """Filtered awareness of what the person is doing: window events with exclusions, activity
history, clipboard on request, microphone use by other apps, saved-file observations and the
schedule for permitted screen awareness."""
D.update({
"Perception/activityHistory": "Bounded history of foreground activity spans.",
"Perception/clipboardText": "Reads clipboard text when the person asks about it.",
"Perception/microphoneUse": "Whether another app is using the microphone (in a call).",
"Perception/perceptionSettings": "Perception settings.",
"Perception/savedFileObservation": "Validated observation of a saved source file from the editor bridge.",
"Perception/screenAwarenessSchedule": "Schedules ambient screen-awareness attempts and keeps the latest result.",
"Perception/windowEventMonitor": "Watches window events and filters excluded apps and titles.",
})

N["Attachments"] = """Files the person gives Revia: intake and validation of images, documents and videos, a private
attachment library, document text extraction (PDF, DOCX, text) with chunking and search, video
decoding through FFmpeg, and selection of which stored media a message refers to."""
D.update({
"Attachments/attachmentIntake": "Validates and stores incoming files (kind detection, image inspection, limits).",
"Attachments/attachmentLibrary": "Private library of attachments: staging, scoped selection, revalidation, video and generated-media import.",
"Attachments/attachmentPrompt": "Renders attached files and document search results into the prompt.",
"Attachments/attachmentTypes": "Attachment kinds, limits and intake results.",
"Attachments/documentChunker": "Splits documents into overlapping chunks.",
"Attachments/documentStore": "Stores documents, their chunks and embeddings; search and forgetting.",
"Attachments/documentText": "Extracts text from text files, DOCX and PDF.",
"Attachments/generatedMediaImport": "Imports images and videos Revia generated into the library.",
"Attachments/inflate": "DEFLATE/zlib decompression (for PDF and DOCX).",
"Attachments/mediaSelection": "Chooses which conversation images a message refers to.",
"Attachments/pdfText": "A small PDF parser for text extraction.",
"Attachments/videoCoverage": "Describes what a video sample did not cover.",
"Attachments/videoDecoder": "FFmpeg-based probing, frame sampling, audio extraction and preview.",
"Attachments/videoSelection": "Recognizes video references and time intervals in a request.",
"Attachments/zipArchive": "Reads an entry from a ZIP archive (for DOCX).",
})

N["Visual"] = """Things Revia makes to look at: sanitized SVG diagrams, local image generation through a loopback
Python worker, local video generation (LTX) with verified artifacts, and the policies that
recognize drawing or video requests."""
D.update({
"Visual/drawingRequestPolicy": "Recognizes drawing requests and their subject.",
"Visual/imageArtifact": "Verifies a generated image file matches what was requested.",
"Visual/imageExecutor": "Action executor for image generation.",
"Visual/imageGenerator": "Owns the image worker process and image jobs.",
"Visual/imageRequest": "Image generation request types.",
"Visual/imageSettings": "Image generation settings.",
"Visual/imageTypes": "Image generation result.",
"Visual/svgCanvas": "SVG sanitizing and the diagram store.",
"Visual/svgValidation": "Strict SVG validation through expat.",
"Visual/videoExecutor": "Action executor for video generation.",
"Visual/videoGenerator": "Owns local video generation jobs and their worker.",
"Visual/videoRequest": "Video generation request types.",
"Visual/videoRequestPolicy": "Recognizes video generation requests.",
"Visual/videoSettings": "Video generation settings.",
"Visual/videoTypes": "Video artifact expectation, receipt and verification.",
})

N["Presence"] = """Revia outside the main window: the avatar state file a Live2D/VRM renderer reads, bounded inbox
adapters for Discord, streams and games, the local HTTP API for trusted local programs, and the
web guest runtime."""
D.update({
"Presence/adapterArchivePolicy": "Selects expired adapter archive files to prune.",
"Presence/localApiRuntime": "The local HTTP API (editor observations and more) for trusted local programs.",
"Presence/localApiSettings": "Local API settings.",
"Presence/presenceRuntime": "Reduces runtime events into avatar state and runs adapter inboxes and replies.",
"Presence/presenceSettings": "Presence settings.",
"Presence/webGuestRuntime": "Runtime for the public web demo guest.",
})

N["Presentation"] = """Typed presentation events (speaking, thinking, gaze, idle motion) and the avatar state they reduce
to, published on a bus that renderers subscribe to."""
D.update({
"Presentation/avatarState": "Avatar state and the controller that applies presentation events.",
"Presentation/debugPresentationSink": "Text sink that records presentation events for tests.",
"Presentation/presentationBus": "The presentation event bus and runtime-event translation.",
"Presentation/presentationEvents": "Presentation event kinds.",
})

N["Performance"] = """Singing: a song library of local assets, WAV mixing, and a runtime that plays one song on its own
device and publishes performance events."""
D.update({
"Performance/performanceRuntime": "Rehearses and performs a song, publishing performance events.",
"Performance/performanceSettings": "Performance settings.",
"Performance/songLibrary": "Lists, validates and loads songs from the library folder.",
"Performance/songTypes": "Song, section, vocal span and performance types.",
"Performance/wavAudio": "WAV reading, mixing, gain and vocal span detection.",
})

N["Content"] = """The working document: blocks with precise, single-block edits and undo."""
D.update({"Content/workingDocument": "A block-structured document with block-scoped edits, undo and rendering."})

# ---- part c ----
N["Actions"] = """The action pipeline every side effect goes through: a typed `ActionRequest` is parsed, evaluated
by capability policy (risk, consequence, confirmation), executed by a narrow executor registered
with the dispatcher, and audited. Why: one admission path for files, processes, desktop control,
browser, internet, images and the guest VM."""
D.update({
"Actions/IActionExecutor": "Interface every action executor implements (Handles, Execute).",
"Actions/actionDispatcher": "Routes an admitted action to the executor that handles it.",
"Actions/actionRuntime": "Parses, evaluates, confirms, executes and audits actions; binds authority and task contracts.",
"Actions/actionRuntimeCapabilities": "Capability editing through the action runtime (approved apps, controls, internet, desktop approval).",
"Actions/actionTypes": "Action, risk, consequence, verdict, target and desktop input types.",
})

N["Planning"] = """Turning words or model JSON into one typed action or goal: structured action parsing, goal
planning schemas, operate intents, quoted payloads, and reminders."""
D.update({
"Planning/goalPlanner": "Goal planning prompts, schemas and parsing of plans and next steps.",
"Planning/operateIntent": "Detects requests to operate the computer or send external messages.",
"Planning/quotedText": "Extracts quoted payloads from a request.",
"Planning/reminders": "Parses reminder requests and keeps the reminder book.",
"Planning/structuredActionParser": "Parses commands and model JSON into a typed action.",
})

N["Policy"] = """Permission decisions: capability settings and their editing, path normalization and approved roots,
desktop authorization and rate limits, a physical stop for desktop input, one-time approvals,
companion authority ceilings, and guest VM capabilities."""
D.update({
"Policy/actionApproval": "One-time approvals scoped to exact action parameters, with expiry.",
"Policy/capabilityEditor": "Edits capability settings (apps, controls, internet, camera, browser, process, guest).",
"Policy/capabilityPolicy": "Evaluates an action against capability settings and approved roots.",
"Policy/capabilityProjection": "Projects the capability scope a turn or task may use.",
"Policy/companionAuthority": "Per-companion authority: sessions, tasks and grants under the machine ceiling.",
"Policy/desktopActionRateLimiter": "Rate-limits desktop input actions.",
"Policy/desktopAuthorization": "Authorizes desktop operations from effect, evidence and approval.",
"Policy/desktopInputGuard": "Emergency stop for synthesized desktop input.",
"Policy/guestCapabilityJson": "JSON form of guest VM capabilities.",
"Policy/guestCapabilityPolicy": "Evaluates guest VM operations against its capabilities.",
"Policy/permissionStore": "Loads capabilities.json with environment-variable expansion.",
})

N["Audit"] = """The record of what happened: action intent and result JSONL written before and after execution,
a durable evidence journal with recovery, cloud request and MCP tool audits, and content digests."""
D.update({
"Audit/actionAuditLogger": "Writes action intent and result records.",
"Audit/cloudRequestAudit": "Audit records for cloud requests.",
"Audit/contentDigest": "SHA-256 content digests.",
"Audit/evidenceJournal": "Durable append-only evidence journal with recovery and health.",
"Audit/mcpToolAudit": "Audit records for MCP tool calls.",
"Audit/runtimeEvidenceBridge": "Copies runtime events into the evidence journal.",
})

N["Filesystem"] = """File operations within policy-resolved paths: read, list, write, copy, move, recycle."""
D.update({
"Filesystem/fileSystemExecutor": "Executes file actions on policy-resolved paths.",
"Filesystem/guardedTextWrite": "Writes text only if the file is unchanged since it was read.",
})

N["Process"] = """Launching programs: typed executable and arguments, bounded output, cancellable child-process
trees, and Windows child-process support."""
D.update({
"Process/ownedProcess": "An owned child process with pipes, termination and exit status.",
"Process/processExecutor": "Action executor for running a typed process request.",
"Process/processTypes": "Process settings, request and result types.",
"Process/windowsChildSupport": "Windows handles, quoting and environment for child processes.",
})

N["Windows"] = """Acting on the Windows desktop: UI Automation lookup, binding a vision-proposed region to an exact
control, synthesized pointer and keyboard input into a verified foreground window, and discovery of
an application's controls."""
D.update({
"Windows/applicationControlDiscovery": "Lists the controls of the foreground application for approval.",
"Windows/applicationLocator": "Resolves an application name to its executable.",
"Windows/desktopControlExecutor": "Executes pointer and keyboard actions into a verified window.",
"Windows/desktopObserver": "Observes the foreground window's controls for decisions.",
"Windows/disposableApplicationFixtures": "Disposable test applications for desktop-control tests.",
"Windows/targetBinding": "Binds and revalidates a target control or point before acting.",
"Windows/uiaElementLocator": "UI Automation lookup of windows and controls.",
"Windows/visionUiaResolver": "Resolves a vision-proposed region to a UI Automation element.",
"Windows/windowsAutomationExecutor": "Executes UI Automation pattern actions.",
})

N["Computer"] = """Operating the computer step by step. For each step a provider decides what to do: routine
rules, a learned ranker, or the legacy model policy. Main's proposed subgoals are validated against
the task's scope; the person's exact content is held in a vault until it is placed; an opt-in
recorder keeps experiences for training the ranker (`Tools/Computer`)."""
D.update({
"Computer/artifactDigest": "Digests for learned-policy artifacts.",
"Computer/computerBenchmark": "Standard benchmark cases for computer-control providers.",
"Computer/computerController": "Chooses which provider decides each step and records decisions.",
"Computer/computerPolicy": "Interface of a computer-control provider.",
"Computer/computerSettings": "Computer control settings.",
"Computer/computerSubgoal": "A validated subgoal Main proposes, with its authority.",
"Computer/computerTaskCoordinator": "Coordinates a computer task: subgoals, observation, payloads, completion.",
"Computer/computerTypes": "Observation, attempt, task context and decision types.",
"Computer/contentGate": "Holds the person's exact content until it is placed where it belongs.",
"Computer/experienceRecorder": "Opt-in recorder of decisions and outcomes for training.",
"Computer/learnedPolicy": "Loads and runs the learned candidate ranker.",
"Computer/legacyLlmPolicy": "The original model-driven computer policy.",
"Computer/observationBuilder": "Builds candidate controls from an observation.",
"Computer/payloadVault": "Holds payloads by reference until redeemed.",
"Computer/routinePolicy": "Rule-based provider for routine steps.",
"Computer/subgoalPlanner": "Formats subgoal requests for Main and parses replies.",
"Computer/subgoalValidator": "Validates a subgoal against the task's scope.",
"Computer/targetMatch": "Matches a described target to an observed control.",
"Computer/taskAcceptance": "Verifies exact content placement at task completion.",
"Computer/taskContent": "Extracts the exact content a task must place.",
"Computer/taskProgression": "Derives task phase and progress.",
})

N["Goals"] = """Multi-step goals: plans persisted with checkpoints, a runner that executes bounded steps with
confirmation and recovery, a sandbox rehearsal for risky file or desktop steps, and outcome
verification through postconditions."""
D.update({
"Goals/goalRunner": "Runs a goal's steps with confirmation, recovery and verification.",
"Goals/goalSandbox": "Rehearses file and desktop steps in a sandbox first.",
"Goals/goalStore": "Persists goals and attempts.",
"Goals/goalTypes": "Goal, step, budget, postcondition and verification types.",
})

N["Browser"] = """The interactive browser: a private Edge profile driven over CDP by `Tools/Browser`, with
generation-bound observation receipts and approved origins."""
D.update({
"Browser/browserExecutor": "Action executor for interactive browser requests.",
"Browser/browserSession": "Owns the browser worker process and its observations.",
"Browser/browserTypes": "Browser settings, request, element and receipt types.",
})

N["Internet"] = """Bounded lookups: deciding when a lookup helps, querying fixed approved HTTPS endpoints
(DuckDuckGo, Wikipedia), and an optional visible browser for search."""
D.update({
"Internet/internetBackend": "Display names of lookup backends.",
"Internet/internetLookupPolicy": "Decides whether a turn needs a lookup and what to ask.",
"Internet/internetSearchExecutor": "Queries approved search endpoints and parses results.",
"Internet/lookupQueryResolver": "Turns a request into a lookup query.",
"Internet/visibleBrowserClient": "Client for searching through the visible browser.",
"Internet/visibleBrowserProcess": "Owns the visible browser host process.",
})

N["Workspace"] = """The guest workspace: an opt-in Ubuntu VM (VMware) Revia can work in, with typed guest requests,
receipts and project sharing."""
D.update({
"Workspace/guestReceipt": "Parses artifacts returned from the guest.",
"Workspace/guestTypes": "Guest operation, request, limits and result types.",
"Workspace/projectShare": "Plans which project files to share with the guest.",
"Workspace/virtualMachineExecutor": "Action executor for guest operations.",
"Workspace/virtualMachineRuntime": "Starts, stops and supervises the guest VM.",
"Workspace/virtualMachineSettings": "Loads and saves guest VM settings.",
"Workspace/vmwareProvider": "VMware implementation of the VM provider.",
})

N["Mcp"] = """Model Context Protocol servers the owner configures: stdio or HTTP transport, tool listing and
calls, schema normalization, confirmation and audit."""
D.update({
"Mcp/mcpClient": "MCP client: initialize, list and call tools.",
"Mcp/mcpManager": "Manages configured MCP servers and their chat tools.",
"Mcp/mcpSchema": "Normalizes MCP tool names and schemas for chat.",
"Mcp/mcpSettings": "MCP server settings and validation.",
"Mcp/mcpTransport": "Stdio and HTTP transports for MCP.",
})

N["Skills"] = """Narrow local integrations packaged as skills: manifests, cases, publishing, activation and rollback,
plus the built-in workspace status skill."""
D.update({
"Skills/packageStorage": "Safe storage helpers for skill packages.",
"Skills/reviaSkill": "The skill interface: capabilities, events, observations, proposals.",
"Skills/skillManager": "Runs active skills and forwards their proposals through admission.",
"Skills/skillPackage": "Skill packages: manifest, cases, verification, publish and rollback.",
"Skills/workspaceStatusSkill": "Built-in skill that reports workspace status.",
})

N["Resources"] = """Hardware and load: inventory of GPUs and memory, one immutable placement plan for models and voice
workers, live usage sampling, a load governor, and the runtime lease that keeps one companion on
the foreground devices."""
D.update({
"Resources/loadGovernor": "Assesses load against thresholds.",
"Resources/resourceMonitor": "Samples usage against the plan and reports pressure.",
"Resources/resourcePlanner": "Inventories hardware and plans model and worker placement.",
"Resources/resourceSettings": "Resource settings.",
"Resources/resourceUsage": "Reads process, memory and GPU usage.",
"Resources/runtimeLease": "Exclusive lease on the foreground model and device runtime.",
})

N["Diagnostics"] = """A bounded issue log of failures and their resolution."""
D.update({"Diagnostics/issueLog": "Records, resolves and compacts issues."})

# ---- part d ----
# Desktop (Qt 6). Keyed by stem; .h/.cpp pairs are one unit.
D.update({
"Desktop/reviaWindow": "The main window: builds the interface from reviaWindow.ui, binds the session, routes runtime events to panels, runs the chat and every studio operation.",
"Desktop/windowShell": "The shell around the pages: panel windows, Settings dialog and backend, left sidebar, chat column, command palette and shortcuts (implements ReviaWindow).",
"Desktop/companionStudio": "Studio panels (Agent, Audience, Learning, Development) and their operations (implements ReviaWindow).",
"Desktop/windowLayout": "Responsive reflow of each window by its own size (implements ReviaWindow).",
"Desktop/main": "Desktop entry point: Qt application, runtime root argument, crash handling, restart handoff.",
"Desktop/agentStudioPanel": "Agent Studio panel: workflow nodes, attempts, delegation origin, decisions.",
"Desktop/answerFeedbackDialog": "Dialog to judge a displayed reply, optionally keeping its exchange for training.",
"Desktop/attachmentTray": "Tray of pictures and files attached to the next message.",
"Desktop/audienceStudioPanel": "Audience Studio panel: people, speaker enrollment and evidence.",
"Desktop/canvasPanel": "Shows diagrams and images Revia drew or was asked to display.",
"Desktop/capabilityPanel": "Permissions panel: approved applications, controls, internet, camera, browser, process and guest settings.",
"Desktop/chatMarkdown": "Renders a reply's Markdown as HTML for the transcript.",
"Desktop/chatSessionsPanel": "Sidebar list of past conversations: new, search, rename, delete, export.",
"Desktop/commandPalette": "Ctrl+K command palette over everything the window can do.",
"Desktop/composerCard": "The message composer card: context row, attachment tray, input and buttons.",
"Desktop/conversationStatusPanel": "Current-state line and owner feedback outcome under the conversation.",
"Desktop/desktopObjectNames": "Registry of widget object names (used by tests and styling).",
"Desktop/developmentStudioPanel": "Development Studio panel: presentation change proposals and review.",
"Desktop/documentsDialog": "Dialog listing kept documents: view or forget.",
"Desktop/internetActivityPanel": "Read-only view of internet lookups.",
"Desktop/learningStudioPanel": "Skills & Learning panel: lesson candidates and training corrections with export.",
"Desktop/memoryPanel": "Memory panel: read-only records, owner revisions and correction proposals.",
"Desktop/mindPanel": "Mind panel: emotion, mood, development, relationships and drives (read-only).",
"Desktop/panelWindow": "Detachable panel windows: placement beside chat, locking, sizing.",
"Desktop/pipelinePanel": "Pipelines tab: state of each runtime lane (read-only).",
"Desktop/profilePanel": "Profiles panel: edit profiles, activate, assign voices.",
"Desktop/resourcePanel": "Resources tab: hardware inventory, placement plan, live usage, voice GPU choice.",
"Desktop/runtimeRootArgument": "Parses the --runtime-root startup argument.",
"Desktop/serialWorker": "One background thread that runs posted work in order (used by Settings writes).",
"Desktop/settingsCatalog": "The single catalog of user-facing on/off settings.",
"Desktop/settingsDialog": "The Settings window: sections, switches, search, live status per row.",
"Desktop/settingsSessionBackend": "Adapter between the Settings dialog and the session.",
"Desktop/settingsTypes": "Settings dialog types (stores, rows, statuses).",
"Desktop/shellSidebar": "The left sidebar: brand, chat list, companion menu, Settings.",
"Desktop/shortcutSheet": "Ctrl+/ keyboard shortcut sheet.",
"Desktop/softShadow": "Soft drop shadow painting for cards.",
"Desktop/studioDuration": "Formats durations for studio panels.",
"Desktop/tabNavigation": "Section-style look for tab widgets.",
"Desktop/theme": "Light/dark theme palette with checked contrast ratios.",
"Desktop/toastOverlay": "One-line toast notices above the composer.",
"Desktop/toggleSwitch": "Toggle switch rendering for permission check boxes.",
"Desktop/videoPreviewDialog": "Plays a stored video preview after admission.",
"Desktop/visionPanel": "Vision panel: monitors, cameras and what Revia may look at.",
"Desktop/voiceHealthPanel": "Voice health badge and details while synthesis is degraded.",
"Desktop/reviaWindow.ui": "Qt Designer layout of the main window.",
"Desktop/revia.qss": "Base stylesheet.",
"Desktop/reviaTheme.qss": "Theme stylesheet (palette tokens).",
"Desktop/reviaContrast.qss": "High-contrast stylesheet.",
"Desktop/reviaResources.qrc": "Qt resource list (stylesheets and icons).",
"Desktop/*.svg": "Check, radio, chevron and spin icons in light and dark variants.",
})

# Tools
D.update({
"Tools/qwen_tts_service.py": "Loopback Qwen3-TTS worker the C++ speech service drives (voices, synthesis, vocalizations).",
"Tools/qwen_lowlatency.py": "Low-latency replacement for Qwen3-TTS's codebook predictor loop.",
"Tools/revia_image_service.py": "Loopback image-generation worker (sd-turbo by default).",
"Tools/revia_video_worker.py": "One local LTX video job under the native owner's process tree and deadline.",
"Tools/revia_training_worker.py": "Optional offline LoRA training job for a reviewed dataset (prepare and run).",
"Tools/revia_adapter_export.py": "Converts a base checkpoint and a trained adapter to llama.cpp files and checks them.",
"Tools/revia_adapter_evaluation.py": "Evaluates a trained adapter against its base (plan, run arms, review, score).",
"Tools/revia_dataset_capture.py": "Turns recorded failures and live captures into dataset candidates.",
"Tools/install_training_assets.py": "Installs pinned Hugging Face snapshots for training or speculation.",
"Tools/CreateReviaClass.py": "Scaffolds a new Public/Private class pair following the project conventions.",
"Tools/Setup-Revia.ps1": "One-command setup: installs runtimes and models for a chosen profile.",
"Tools/Build.ps1": "Builds Revia (CMake + Ninja, MinGW) with build-tree repair and optional tests.",
"Tools/RunDesktop.ps1": "Starts the desktop build.",
"Tools/HealthCheck.ps1": "Checks an installation's runtimes and models.",
"Tools/ReviaAcceleration.ps1": "Detects the best available accelerator (CUDA, Vulkan, CPU).",
"Tools/InstallLlamaCpp.ps1": "Installs the llama.cpp runtime matching the accelerator.",
"Tools/InstallQt.ps1": "Installs Qt for the desktop build.",
"Tools/InstallQwenTTS.ps1": "Installs the Qwen3-TTS Python environment.",
"Tools/InstallWhisper.ps1": "Installs whisper.cpp and the small.en model.",
"Tools/InstallImageModel.ps1": "Installs the image-generation environment and model.",
"Tools/InstallVideoDecoder.ps1": "Installs FFmpeg/ffprobe for video understanding.",
"Tools/InstallVisionProjector.ps1": "Installs the vision projector (mmproj) for the chat model.",
"Tools/InstallMemoryNli.ps1": "Installs the optional CPU NLI worker.",
"Tools/InstallMemoryReranker.ps1": "Installs the optional CPU reranker worker.",
"Tools/DownloadChatModel.ps1": "Downloads the chat GGUF and updates settings.json.",
"Tools/DownloadEmbeddingModel.ps1": "Downloads the embedding model.",
"Tools/DownloadRuntimeModels.ps1": "Installs the latency-first model stack for the target machine.",
"Tools/SelectInstalledMainModel.ps1": "Switches Main to another installed model (DryRun/Apply with receipts).",
"Tools/Benchmark-ReviaTTS.ps1": "Benchmarks TTS latency (baseline vs optimized).",
"Tools/RunArchitectureEvaluation.ps1": "Runs the architecture evaluation with digested artifacts.",
"Tools/RunCognitionLiveCampaign.ps1": "Runs a live cognition campaign against a built quality tool.",
"Tools/VerifyStudioCandidate.ps1": "Verifies a studio build candidate's artifacts.",
"Tools/VerifyWorkbench.ps1": "Builds and tests the improvement workbench copy.",
"Tools/Build/InvokeCheckedCTest.ps1": "CI: runs CTest and checks the full test inventory and approved skips.",
"Tools/Build/InvokeMeasuredBuild.ps1": "CI: measured build with a source snapshot.",
"Tools/Build/FileHash.ps1": "File hash helper.",
"Tools/Build/ProcessTree.ps1": "Process tree helper.",
"Tools/Build/ctest-windows-expected.txt": "The expected Windows CTest inventory (CI gate).",
"Tools/Workers/memory_nli.py": "Optional CPU entailment worker for memory duplicates.",
"Tools/Workers/memory_reranker.py": "Optional CPU reranker worker for memory search.",
"Tools/Workers/test_memory_nli.py": "Contract tests for the NLI worker.",
"Tools/Workers/test_memory_reranker.py": "Contract tests for the reranker worker.",
"Tools/Computer/dataset.py": "Reads and splits computer-experience recordings.",
"Tools/Computer/features.py": "Features for the learned candidate ranker.",
"Tools/Computer/parity.py": "Checks training and deployment compute the same features.",
"Tools/Computer/train.py": "Trains and packages the candidate ranker.",
"Tools/Browser/browserHost.mjs": "Node host for the visible search browser.",
"Tools/Browser/interactiveHost.mjs": "Node host driving the interactive Edge browser over CDP.",
"Tools/Browser/browserHost.test.mjs": "Tests for the browser host.",
"Tools/Browser/interactiveHost.test.mjs": "Tests for the interactive host with a scripted CDP peer.",
"Tools/Browser/interactiveHost.live.test.mjs": "Live test of the interactive host.",
"Tools/Browser/interactiveNative.live.test.mjs": "Live test of the native interactive browser.",
"Tools/Guest/guest_worker.py": "Runs inside the guest VM: one confined request per SSH command.",
"Tools/Guest/vmware_ubuntu.py": "Enrolls and manages the VMware Ubuntu workspace.",
"Tools/Guest/windows_sandbox.py": "Prepares Windows Sandbox mounts.",
"Tools/Guest/guest-bootstrap.ps1": "Guest bootstrap with fixed paths.",
"Tools/Guest/test_guest_worker.py": "Tests for the guest worker.",
"Tools/Guest/test_vmware_ubuntu.py": "Tests for VMware enrollment.",
"Tools/Quality/answerGateLive.cpp": "ReviaAnswerGateLive: runs the golden set through the real runtime against a model.",
"Tools/Quality/answerQualityLive.cpp": "ReviaAnswerQualityLive: runs the conversation-contract corpus or holdout against a model.",
"Tools/Quality/companionAwarenessLive.cpp": "Live probe of companion awareness on a small model.",
"Tools/Quality/companionImageLive.cpp": "Live probe of image generation through the companion.",
"Tools/Quality/companionVisionLive.cpp": "Live probe of vision through the companion.",
"Tools/Quality/mainVisionSetting.cpp": "Inspects and sets Main's vision setting.",
"Tools/Quality/datasetExportTool.cpp": "ReviaDatasetExport: writes a dataset export from a candidate file.",
"Tools/Quality/observedLocalModel.h": "Loopback proxy that records traffic to a model for evaluations (authenticated).",
"Tools/Quality/localModelCredentials.h": "Operator-selected credential source for evaluation tools.",
"Tools/Quality/answerGateExperiment.h": "Answer-gate experiment options (prompt-block ablations).",
"Tools/Quality/cognitionLive.h": "Cognition campaign helpers for live tools.",
"Tools/Quality/privateRuntimeEvaluation.h": "Single-turn private-runtime evaluation setup.",
"Tools/Quality/routerJudge.h": "Default judge through the local model tiers.",
"Tools/Quality/semanticReview.h": "Review packet types for human semantic review.",
"Tools/Quality/answerGateSession.py": "Runs an answer-gate campaign on an owned model.",
"Tools/Quality/answerGateSessionTests.py": "Tests for the answer-gate session runner.",
"Tools/Quality/contextBenchmark.py": "Continuity probes against llama.cpp for context benchmarking.",
"Tools/Quality/createCognitionFixtures.py": "Authors the frozen cognition corpus.",
"Tools/Quality/nliDedupBenchmark.py": "Offline NLI duplicate-detection quality assay.",
"Tools/Quality/repairCodingOracle.py": "Runs the repair cohort's acceptance checks.",
"Tools/Quality/retrievalBenchmark.py": "Reconstructed LoCoMo retrieval benchmark.",
"Tools/Quality/retrievalBenchmarkTests.py": "Tests for the retrieval benchmark.",
"Tools/Presence/SendPresenceEvent.ps1": "Sends one presence event to Revia's inbox.",
"Tools/Presence/WatchPresenceReplies.ps1": "Watches an adapter's reply outbox.",
"Tools/Presence/Send-EditorObservation.ps1": "Sends a saved-file observation to the local API.",
"Tools/Presence/Watch-EditorFile.ps1": "Watches a file and sends observations when it is saved.",
"Tools/Presence/Live2D/main.mjs": "Live2D renderer bridge: follows avatar state.",
"Tools/Presence/Live2D/client.mjs": "Live2D client connection to the renderer.",
"Tools/Presence/Live2D/state.mjs": "Avatar state handling for Live2D (mouth bounds).",
"Tools/Presence/Live2D/StartLive2D.ps1": "Starts the Live2D bridge.",
"Tools/Presence/DiscordVoice/main.mjs": "Discord voice adapter entry point.",
"Tools/Presence/DiscordVoice/adapter.mjs": "Discord voice adapter: audio to whisper, replies back.",
"Tools/Presence/DiscordVoice/transport.mjs": "Connection ownership for the Discord transport.",
"Tools/Presence/DiscordVoice/adapter.test.mjs": "Adapter tests.",
"Tools/Presence/DiscordVoice/transport.test.mjs": "Transport tests.",
"Tools/Presence/WebDemo/WebDemo.ps1": "Operator script for the public web demo (start, bridge, check...).",
"Tools/Presence/WebDemo/bridge/client.js": "Bridge between the relay and the native web guest.",
"Tools/Presence/WebDemo/bridge/main.js": "Bridge entry point.",
"Tools/Presence/WebDemo/operator/config.js": "Operator configuration (parsed as data).",
"Tools/Presence/WebDemo/operator/main.js": "Operator entry point.",
"Tools/Presence/WebDemo/protocol/index.js": "Web demo protocol schema.",
"Tools/Presence/WebDemo/relay/config.js": "Relay configuration.",
"Tools/Presence/WebDemo/relay/main.js": "Relay entry point.",
"Tools/Presence/WebDemo/relay/proxy.js": "Trusted proxy handling for client addresses.",
"Tools/Presence/WebDemo/relay/server.js": "Public relay server with bounded per-visitor tables.",
"Tools/Presence/WebDemo/test/*": "Web demo tests: bridge, config, operator, protocol, proxy, relay, smoke and integration.",
"Tools/Presence/WebDemo/*": "Web demo READMEs, Docker and Render deployment files, env examples and protocol fixtures.",
"Tools/Presence/*": "Presence adapter READMEs, package manifests and example configs.",
"Tools/Studio/CMakeLists.txt": "Standalone build of the studio-duration check against a host-approved Development Studio candidate.",
"Tools/Quality/nliDedupBenchmark.md": "How to run the NLI duplicate-detection assay.",
"Tools/Quality/retrievalBenchmark.md": "Notes on the reconstructed LoCoMo retrieval experiment.",
"setup.bat": "Double-click setup: runs Tools/Setup-Revia.ps1 with PowerShell.",
"Tools/CodeMap/build_code_map.py": "Generates docs/CODE_MAP.md; --check reports a stale map or a file without a description.",
"Tools/CodeMap/descriptions.py": "The hand-written descriptions and domain narratives the code map is built from.",
})

# Config, assets, build and repository files
D.update({
"Config/settings.json": "Checked-in default settings for every domain (owner overrides live in RuntimeData).",
"Config/capabilities.json": "Default capability settings (what Revia may do).",
"Config/model_manifest.json": "Model profiles and pinned artifacts (repository, revision, sha256).",
"Config/training_assets.json": "Pinned training assets for the optional training pipeline.",
"Config/avatar.json": "Avatar configuration for presence.",
"Config/Profiles/*": "Companion profiles (revia, assistant): system prompt, sampling, personality.",
"Config/CompanionSeeds/*": "Seed descriptors for new companions.",
"Config/Defaults/*": "Starter RuntimeData copied on first run (the default voice and its vocalizations).",
"Config/Evaluation/*": "Evaluation corpora: golden set, answer-quality corpus and holdout, adapter tool cases, architecture tests.",
"Config/Skills/*": "Built-in skill packages (workspace inventory, two versions).",
"Assets/*": "Avatar artwork: Live2D parts, rigs and sources.",
"Models/*": "Model notes (models themselves are installed, not tracked).",
"CMakeLists.txt": "The build: ReviaFoundation static library, executables, desktop, tests and CTest registrations.",
"CMakePresets.json": "CMake presets.",
".github/workflows/build-and-test.yml": "CI: Windows MinGW build with every test, Linux journal check, voice/web/operator jobs.",
".github/workflows/nightly-eval.yml": "Nightly evaluation workflow.",
"README.md": "Project overview and setup.",
".clang-format": "C++ formatting rules.",
".gitignore": "Ignored paths.", ".gitattributes": "Git attributes.",
".claude/*": "Claude Code session files.", ".idea/*": "IDE project files.",
"build-fix-check.log": "A tracked build log (probably accidental).",
})
