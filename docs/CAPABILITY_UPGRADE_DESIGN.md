# Revia continuity and capability upgrade design

Status: approved implementation design. Current delivery and verification are
recorded in [the integration ledger](superpowers/plans/2026-10-06-capability-integration.md).
The following audit findings were confirmed on 2026-10-06 at
`b2fc080e3f2af46f9097559051310e9f80502846`. They describe the starting point, not
the resulting implementation. No running Revia process was observed
during this audit. Checked-in configuration is not proof of settings used in the
owner's earlier session. Existing milestone acceptance records retain their
original scope and evidence.

## Product intent

Improve conversational continuity and useful independent work while preserving
Revia's authored personality, relationships, voice and avatar. Add genuine art
generation and reliable command/computer tools under the existing owners. Use
local models by default; keep provider boundaries open for optional alternatives.
Feature breadth and measured task quality are separate acceptance criteria.
Parity with every commercial assistant or game specialist is not established.

The governing sources are the owner's requests, `AGENTS.md`,
[architecture boundaries](ARCHITECTURE.md), and the supplied Studio v3 control
document. This proposal extends those owners rather than adding another memory
database, personality engine, scheduler or generic executor.

## Findings and current boundaries

| Area | Source-confirmed state | Missing responsibility |
|---|---|---|
| Context | Main is configured for 8,192 tokens and 1,024 output tokens. `LLM/tokenEstimate` uses UTF-8 byte length as a conservative token bound. `LLamaCPP/llamaCppService` reserves another 384 tokens and framing. Under overflow, the system message can consume 70% of the available budget. | Accurate backend/template token accounting and visibility into discarded context. Do not replace the conservative bound with an average characters-per-token guess. |
| Continuity | `Core/conversationContext` keeps up to 24 messages, a 14,000-byte history budget and a 2,400-byte rolling summary. Older messages contribute their first 280 bytes. The request fitter can discard additional recent history without updating that summary. | Task/topic continuity that retains decisions, corrections and unfinished work with source references. |
| Recall | `LLM/promptBuilder` queries memory using the latest user message; brief social turns omit retrieved memories and restrict history. Restart restoration is a configurable archive tail, currently six messages. | Contextual follow-up retrieval and scoped persisted continuity across restart. |
| Person facts | `Memory/memoryTypes` entries and classification decisions have no participant/entity field. Relationships already have person ownership. | Preserve subject attribution through classification, storage, recall and correction; retain legacy facts as unattributed rather than guessing an owner. |
| Routing | `ConversationRuntime` stores `routeDecision.selectedTier`, the selection made before generation, rather than the router's delivered `output.selectedTier`. | Follow-ups should inherit the model tier that actually delivered the answer after fallback. |
| Commands | `Actions/actionTypes` and `ActionRuntime` support bounded file/desktop operations but no general process tool with arguments, output, exit status and cancellation. Terminal typing permission is a separate existing capability. | A host-owned process execution tool and controlled file editing, carried through the same authority/audit path. |
| Computer operation | `/operate` connects a real goal loop and UI Automation observations. Separate vision analysis exists. `NeedVision`, `WaitForState` and `Reobserve` do not drive their intended recovery paths. A provider completion proposal can mark a general goal successful without a separate final acceptance check. | A shared screenshot/UIA observation loop, real recovery dispatch and independent whole-task acceptance. |
| Browser | The dedicated browser worker supports bounded public read-only research, not a general stateful browser tool. | An optional interactive browser adapter with its own effective permissions and verified observations. |
| Art | `Tools/revia_image_service.py` runs a real Diffusers pipeline. `DocumentWorkshop::GenerateImage` exposes it through `/imagine`. Image generation is disabled in checked-in defaults; natural drawing requests route to `DrawDiagram`. | Picture-versus-diagram intent, verified outputs, cancellable jobs and shared GPU scheduling/lifetime. |
| Agents and learning | `AgentWorkflow` has dependencies, budgets, review and checkpoints. Its local analytical providers have no general OS tools. Learning has reviewed evidence. SelfDevelopment admits a narrow duration-formatting transformation. | Tool-capable bounded workers and broader qualified experiments; learning, code improvement and model training remain different capabilities. |
| Games/VM | Generic input tools and file/UI fixtures exist. No dedicated game-playing policy or VM-backed desktop adapter was found. | Separate guest execution and domain-specific perception/control policies with measured task results. |

## Delivery sequence

### 1. Conversation continuity

Extend `Core/conversationContext`, `LLM/promptBuilder`, the llama.cpp provider,
`Runtime/conversationRuntime`, and existing Memory/archive owners.

- Obtain the loaded backend's tokenizer and chat-template cost when supported.
  Keep byte-conservative fitting as the fallback and keep context overflow
  recovery. Expose actual/estimated cost, reserve, retained turns and compaction
  reason through existing runtime diagnostics without exposing private text.
- Maintain bounded working continuity: current topic, decisions, constraints,
  corrections and unfinished questions/tasks, referencing admitted source turns.
  A generated summary is lower-trust continuity material, not automatically a
  durable fact. Protect the latest request and useful recent exchanges before
  verbose optional runtime context.
- Query recall with admitted topic and recent context as well as the latest
  input. Preserve captured companion, participant, audience and consent scope
  across asynchronous operations and restart recovery.
- Carry person attribution through durable facts. Migrate conservatively and
  preserve exact revision receipts. Record the actual delivered model tier.
- Benchmark 16K and then 32K context with current concurrent workloads. Do not
  select the model card's maximum merely because it is advertised.

Acceptance: synthetic long conversations with late corrections and decisions;
pronoun-only follow-ups after topic changes; process restart; two participants
with conflicting preferences; multilingual large input; fallback routing;
cancellation and stale-scope rejection. Native contract tests and a fixed live
model corpus are separate. Report continuity accuracy, latency, memory and
personality regressions rather than declaring success from prompt size alone.

### 2. Verified command and computer operation

Extend the existing `ActionRuntime`, Policy, Computer, Goals and Audit owners.
Existing model/browser subprocess owners are purpose-specific. General admitted
process execution is the missing responsibility: give its native lifetime and
output handling one focused Process owner with matching public/private contracts.
Do not place shell execution in Windows, whose current boundary forbids it. Keep
a single desktop-input arbiter and deterministic stop.

- Add typed process requests/results with working directory, executable and
  arguments or an explicitly authorized shell mode, bounded environment, stdout,
  stderr, exit status, timeout, cancellation and session identity. Native code
  owns child lifetime and process-tree cleanup. Expose scoped file patch/write
  operations with expected prior content identity.
- Specify a separate process capability. Current architecture forbids model
  text becoming a shell command; adding an explicit admitted tool requires a
  deliberate contract/invariant update, not bypassing it with terminal typing.
- Build the model-visible tool schema from effective permissions. Have the
  existing iterative provider consume supplied fresh screenshots and UIA
  observations from admitted capture adapters; implement
  wait, reobserve and vision escalation rather than treating them as completion.
- Require runtime evidence for final acceptance: expected files/content, process
  results, application state or other task-specific checks. A model saying
  "finished" is a proposal, not the success receipt.
- Persist attempts and resumable task state. Respect cancellation before effects
  and publication; uncertain effects require reobservation before retry.
- Define interactive browsing as a separate capability and deliberate update to
  architecture invariant 10. Keep the current research worker's bounded contract.
  An admitted browser adapter owns effects, isolated sessions/profiles, observation
  receipts, cancellation and uncertain-effect recovery; effective authority is
  rechecked before each operation. A planner does not inherit a raw browser API.

Acceptance: edit/build/test a disposable project; nonzero exit and timeout;
cancel a child process tree; complete a browser/app task with hidden intermediate
state; recover from a changed window; reject an unsupported completion claim;
resume an interrupted goal without duplicate effects. Record completion rate,
wrong actions, interventions, recovery, latency and model calls on held-out tasks.

### 3. Genuine art integration

Retain `Visual/imageGenerator` as provider owner, `DocumentWorkshop` as artifact
coordinator, ActionRuntime as admission/audit owner and Canvas as consumer.

- Route illustrations and pictures to an image model; route diagrams and UI
  layouts to their existing structured renderers. Explain unavailable generation
  accurately rather than substituting a diagram as successful art generation.
- Add cancellable job state and model lifecycle/resource planning. A long HTTP
  timeout is not cancellation. Verify file ownership, image decoding, dimensions
  and provenance before publishing an artifact; visual prompt satisfaction needs
  separate review beyond file integrity.
- Use the current SD-Turbo worker for a baseline smoke test with provider-correct
  parameters. Benchmark a higher-quality provider behind the same contract,
  including optional host-owned ComfyUI workflows, reference images and editing.
  No arbitrary model-proposed executable workflow is admitted.
- Let existing autonomous creation nominate art work through the same admitted
  job path. Avoid a separate background generation daemon.

Acceptance: a natural-language illustration request produces a decoded raster
artifact; a diagram remains a diagram; missing weights/imports, GPU contention,
cancel, restart and unload have accurate states; repeatable prompts demonstrate
useful art quality and character consistency separately from successful execution.

### 4. Independent operation and specialist skills

Introduce admitted capture/input/process adapters for a disposable guest
environment, compatible with existing Computer/Goals contracts. Platform/service
adapters and their resource/lifecycle owners perform guest effects. Computer
consumes observations and chooses a step; it does not observe or execute. Goals
retains budgets, checkpoints and outcome verification, not OS executors.
The existing file sandbox is not VM isolation.
Guest readiness, visible state, networking, input ownership, shared artifacts,
reset and reconnect need explicit contracts. Windows/GPU compatibility and latency
remain unverified until a real guest is exercised.

Extend existing autonomy nominations and AgentWorkflow with bounded tool-capable
workers, reviewed evidence and parent acceptance. Keep timers/event triggers,
foreground interruption, cumulative budgets and dependency-aware retry with their
existing owners. Use distinct model/tool capabilities per role while keeping the
parent companion's personality and private identity as the final speaker.

Game support is a separately qualified skill: rapid perception, low-latency
controls and a trained/scripted reactive policy, with the LLM supplying slower
strategy. Prove one game's tasks before claiming broader support. Generic mouse
and keyboard access does not establish competent Minecraft, RTS or Megabonk play.

## Hardware and evidence constraints

The host reported RTX 5070 with 12,227 MiB VRAM, RTX 2070 SUPER with 8,192 MiB VRAM
and 128 GiB system RAM. Treat each GPU as a separate placement target. Splitting,
quantization and CPU offload are model/backend-specific and need measurement.
Conversation, speech, vision and art must share a bounded resource plan and retain
foreground responsiveness. Model choice does not establish frontier-level quality.

Primary references checked during this audit:

- [Qwen3.5-4B model card](https://huggingface.co/Qwen/Qwen3.5-4B): advertises 262,144 native context tokens; this does not certify the installed GGUF/backend configuration or practical local latency.
- [Claude computer-use integration](https://platform.claude.com/docs/en/agents-and-tools/tool-use/computer-use-tool): application-owned tool execution and observation loop.
- [SD-Turbo model card](https://huggingface.co/stabilityai/sd-turbo): preferred 512-pixel generation and disabled classifier-free guidance; documents quality limitations.

## Engineering handoff

The Director collected three real parallel source audits: context, computer
capabilities, and art/autonomy. Their findings are inspection evidence, not live
feature acceptance. Execute the four packages incrementally with bounded domain
supervision and independent review; record actual hierarchy/status and unavailable
telemetry honestly. Preserve unrelated IDE/build edits and user data. Integrate
verified approved implementation into main, push it, and remove only the task's
own merged branches under the owner's standing integration instruction.
