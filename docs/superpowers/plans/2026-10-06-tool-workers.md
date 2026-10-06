# Tool-capable local workflow workers

The approved capability design extends the existing four-node AgentWorkflow. Runtime owns model/tool dispatch and captured authority; Agents owns attempts, dependencies, cumulative budgets, artifacts and parent acceptance. The companion remains the final speaker.

## Product contract

- Add `Local with tools` beside the existing local analytical and deterministic providers. Preserve old saved provider selections as analytical mode.
- Only workers may request tools. Parent and reviewer continue consuming bounded evidence and producing the existing strict deliverable contracts.
- Each worker model response is either one typed action or a final deliverable; at most four tool actions per attempt. Allowed actions are directory listing, text reading, digest-checked text writing, and explicitly enabled/granted process execution. Desktop, browser, VM and unrestricted shell tools are excluded from this provider.
- Dispatch through `ActionRuntime::ExecuteScopedFor` using captured companion/session/workflow/attempt authority and a narrow captured policy. Before-effect admission checks cancellation, active attempt, live session and current policy. A configured process ceiling alone does not supply missing parent task authorization.
- Tool outputs are bounded and supplied as untrusted evidence. Host receipts carry action identity, status and content digest. Provider statements do not manufacture receipts or successful checks.
- Provider calls, tool calls and reserved tool-output bytes have cumulative engine-owned budgets that survive cancellation, retry and restart. Reservation is persisted before dispatch; persistence failure denies the effect. Token reports include all model calls made by the attempt.

## Implementation sequence and checks

1. Add failing workflow checks for current-attempt reservations, concurrent shared exhaustion, cancellation and checkpoint round trips. Add bounded persisted counters and admission APIs to the existing owner, retaining schema-2 compatibility.
2. Add failing strict tool-envelope tests: malformed/unknown/additional fields, action/type mismatch, absent write digest, oversized payload, forbidden operations and process grants. Implement a focused Agents helper that parses typed requests and produces bounded host receipt material.
3. Adapt Runtime's provider loop to call the helper, reserve/persist work before each model/tool call, register restricted authority tasks, and retain the existing final deliverable validation. Propagate mode through start/resume/retry/persistence. Root owns session header and the UI provider option.
4. Exercise real native read/list/write/process dispatch with fixture authority, expected hash conflicts, cancellation/revocation, cumulative bounds and parent acceptance. Re-run existing AgentWorkflow/authority/action checks through root's shared build, then report actual limits; no live-model performance or capability claims without measurement.

No new scheduler, direct filesystem tool implementation, deployment pathway or private identity context is added. Failed/interrupted attempts retain consumed reservations. The existing dependency invalidation and separate parent acceptance remain authoritative.

## Implementation and evidence

- Runtime now persists the original narrowed tool scope, allows read/list by default, assigns digest-bound writes to the approach worker only when the captured reversible-write ceiling permits them, and assigns processes to the evidence worker only with both captured and current explicit delegated-process flags. Process confirmation derives from those flags; executable/root restrictions and runtime parent/attempt authority remain enforced.
- Tool-mode verification depends on the approach artifact and must return its exact reference. Analytical mode retains independent workers. Host result summaries and receipt digests travel through the existing artifact evidence and review/parent acceptance.
- Schema 3 records per-attempt and cumulative provider/tool/output reservations. Actual reported tokens are charged after each model call, persisted before further calls/effects, and counted only once at completion. Schema 2 remains readable.
- Interrupted/cancelled tool attempts are uncertain effects. Resume refuses them until existing Retry receives fresh observed recovery evidence; changing an objective alone is insufficient. This bounded refusal replaces a new tool-operation journal. Read-only tool reservations are conservatively treated the same way.
- Red checks were missing budget/admission APIs, missing strict tool helper, and missing per-call token accounting. The direct-source executable ran the new strict parsing/receipt checks, concurrent cumulative budgets, token exhaustion/no-double-count, checkpoint corruption/restart/recovery, tool dependency ordering, and the complete existing AgentWorkflow suite successfully. New integrated native tests cover actual list/read/hash-checked writes/processes and stale parent/admission; their execution belongs to root's shared build.

Limits: at most four tool actions per worker attempt; 16 KiB reserved receipt material per action; no desktop/browser tools in workers. Evidence excerpts are bounded and may be truncated, and the existing action audit remains the execution record. Unknown token usage stays unknown; hard call/output/time bounds still apply. No live-model competence, performance, or parity measurement was performed.
