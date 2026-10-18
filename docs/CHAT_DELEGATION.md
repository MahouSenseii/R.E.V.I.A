# Chat delegation

Chat is the main way to talk to Revia, and handing work to her Agent Studio workers is one of the things she can do in it. She stays responsible for the conversation, the task and the final answer. The workers return evidence; she decides what to make of it and says so in her own voice.

This is the first chat-facing slice of W09 in the October 18 improvement plan. It reuses the existing `AgentWorkflow` owner, its four-node template and its review; it does not add a second agent manager.

## Three ways to handle a request

| Approach | When | Mechanism |
|---|---|---|
| Answer directly | The conversation and context are enough | Ordinary reply, no tool |
| Use an ordinary tool | One lookup, memory search, file read or calculation | The existing chat tools |
| Delegate | The request benefits from separate investigation, several independent outputs or an independent check | `delegate_task` |

No extra model call classifies messages. The conversation model chooses `delegate_task` through its tool description; native code decides whether the tool is offered and whether a submission is admitted.

## When `delegate_task` is offered

`conversation.delegation` decides, read live by every chat turn's tool grant and changeable with `/set conversation.delegation <mode>` or the Conversation behavior control:

| Mode | Offered when |
|---|---|
| `off` | Never |
| `on_request` (default) | The person's own words ask for it: "use your agents", "have your workers look into it", "delegate this" |
| `auto` | Always in a private chat turn; the model chooses |

"Answer this directly", "don't use your agents" and similar phrases close it in every mode. The cue is read deterministically from the input (`Agents/delegation`). `task_status` and `cancel_task` are offered whenever a task delegated in this conversation exists, whatever the mode, so turning delegation off never strands a running task. Public, shared and guest audiences get no delegation tools, as with every other tool.

`on_request` stays the default until a qualification run shows that delegated tasks actually beat a direct answer at their cost on the local model.

## What the workers may touch

Delegation is separate from permission to act. A chat task captures the owner's approved folders with **reading only**: no writes, no processes and no command interpreters, whatever else the owner has granted (`tool-scope.json` records `writes: false`, `process: false`). Without approved folders, the workers get no tools and reason from the objective alone. Their authority tasks are registered as narrowing tasks that cannot widen anything.

The workers see the objective Revia wrote, not the conversation or her private memories. The objective therefore has to be self-contained; the tool's schema says so.

## Lifecycle

`delegate_task` returns at once with `{task_id, status: "submitted", access}` and the note that nothing has been checked yet. The task then has its own lifetime:

| Event | Effect |
|---|---|
| A new message | Nothing: the work keeps running and she answers normally |
| The model calls `delegate_task` again on the same turn | Answered with the task already started (`already_submitted`) |
| Another turn tries to delegate while a task runs | Refused, naming the running task |
| Agent Studio holds a workflow awaiting a decision | Refused; chat never discards that work |
| `cancel_task <id>`, the card's Cancel, `/task cancel`, "cancel the task" | Cancels exactly that task; another id is refused |
| Stop | Cancels it, as Stop cancels every background task |
| Agent Studio starts or resumes another workflow | The chat task is marked Replaced and never reports |
| Agent Studio retries or resumes the same task | A new revision, reported afresh. Only these explicit restarts reopen a task; a workflow merely seen running again never does |
| A new chat, continuing or deleting one, or forgetting chats | The task keeps running, but its outcome is never reported into another conversation: it is left in Agent Studio (Report unavailable) |
| The companion session ends | Its workflow is cancelled and joined; nothing is reported later |

Statuses (`DelegatedTaskStatus`): Working, Reviewing, Reporting, Reported, Not established, Cancelled, Rejected, Replaced, Report unavailable. The record, with its origin turn, conversation id, access, budget and revision, is saved to `RuntimeData/Agents/delegation.json`.

The budget is the workflow's own, with five minutes of active time instead of two. Spent budget is never refunded.

## How the result comes back

1. The workers and the reviewer run. When the workflow awaits acceptance, the existing parent check runs as the gate.
2. Agent Studio's automatic acceptance is skipped for chat tasks. Once the parent check has verified the deliverable, the task becomes *Reporting* and its exact artifact (id, version, SHA-256) is bound to it.
3. A background worker waits for the foreground: no turn in progress, not speaking, the person not talking, the same person and audience. Her conversation always comes first. A report preempted by a new message is retried and does not count as a failure.
4. Under the conversation lock it checks again that the task, its revision and the exact artifact are still current, then runs a report turn through `ConversationRuntime::ReportDelegatedTask`. This is her normal reply path, with her identity, history, filters and voice, no tools and no screen context. The person's own words and the reviewed deliverable, with the hosts' receipts, are its grounding, marked as untrusted data.
5. Only after that reply is delivered is the workflow accepted, and only with the artifact she examined (`AgentWorkflow::AcceptCandidate`).

Work the review did not verify, including work that spent its budget before the acceptance check could run, is reported as *not established*, with the stage that stopped and its safe diagnostic. It is never accepted. Quiet mode keeps the report from being spoken but still shows it. Once delivered, the report keeps its voice while the task is marked reported; only a stale outcome (cancelled, replaced, another revision, the session or chat gone) stops it.

## Shared task view

The chat card under the delegating reply and Agent Studio read the same `AgentWorkflowSnapshot()` and `DelegatedTask()` in one refresh, so they cannot disagree. The card shows the task, its state, the active and waiting worker, elapsed time and the next handoff, with **Details** (opens Agent Studio) and **Cancel**. Agent Studio shows that its current workflow came from chat.

## The authority revision change

`CompanionAuthority` used to advance its single policy revision whenever any task was registered or ended, and every chat turn's delivery admission compares its captured revision with the current one. A background worker starting or finishing an attempt therefore revoked whatever reply was being written beside it, including the reply announcing the delegation itself. `RegisterTask` now takes `advancesRevision`. The workflow parent and its per-attempt worker tasks pass `false`. A task can only narrow its session or parent, so adding or ending one cannot change what any other subject may do. Its own stamps are still refused once it ends, by the active-subject check. Grants, revocations, defaults, sessions and every other task keep the old behavior. This also fixes Agent Studio's "Local model with permitted tools" mode, which had the same problem.

## Verification status

| Claim | Status |
|---|---|
| Offering rules, cue reading, schema bounds, setting validation and preference | Tested (`Tests/delegationTests.cpp`, `Tests/contextFittingTests.cpp`) |
| Non-blocking submission, one task for repeated calls, talking during the work, report through her reply, acceptance only after it and only of the examined artifact | Tested against a llama-server stand-in that holds the workers mid-task |
| Cancel by id (including `/task cancel` while the report waits), stale outcomes (cancelled or replaced) never reported, unverified work and a budget spent before review reported as not established | Tested |
| An outcome is never reported into a different chat; a Studio retry reopens the task as a new revision | Tested |
| Workers yield to her replies on one model slot and ask again | Tested; the check fails if the retry is removed |
| The spoken report is not cut off once the task is marked reported | Fixed after review; not covered by an automated test (no speech output in the test harness) |
| Read-only capture with write-capable settings; announcing reply survives tool workers | Tested; the reply check fails if the revision change is reverted |
| Chat card and Agent Studio origin line | Compiled; checked only with the desktop fixture where noted |
| Real local model choosing `delegate_task` well, report quality, benefit over a direct answer, latency on the target machine | **Not yet measured** |
| Auto mode as the default | **Not qualified** |

## Known limits

- One workflow at a time: the existing owner holds one workflow, so a second delegation is refused until the first is over. Nothing is queued.
- Workers get the objective only, not scoped conversation context or memory.
- After a restart the saved record identifies an earlier chat task, but its outcome is not reported into the new session; it stays in Agent Studio.
- The report turn is a proactive turn. If its delivery is revoked after the reply was added to working history, that line can stay in history without having been shown (a pre-existing property of proactive turns).
- Goal tasks (`LaunchTask`) still register advancing authority tasks, so a goal starting or ending mid-reply can still revoke that reply. That is outside this change.
