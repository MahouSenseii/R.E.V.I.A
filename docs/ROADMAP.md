# Roadmap to the desktop-companion vision

The goal is a Revia that lives on the Windows desktop as an embodied character, converses naturally, notices what the user is doing, proposes help unprompted, operates supported applications, pursues bounded goals, tests its work, and eventually handles pre-approved work without constant oversight. That requires staged autonomy; it should not be implemented as unrestricted mouse, keyboard, shell, or administrator control.

## How to read this

The work is not one line. It runs on three tracks that progress at different rates and have different risk profiles:

- **Authority** (Stages 1-5, 7) — what Revia is permitted to do, and how that permission is proven safe. Sequential; each stage depends on the previous.
- **Perception** (Stage 6) — what Revia can observe. Depends on Stage 3. Independent of Stages 4 and 5.
- **Presence** (Embodiment track) — how Revia appears and is interacted with. Depends only on Stage 2's event bus. Buildable entirely in parallel and by someone who never touches the policy layer.

Stage 7 is the convergence point: it needs Stage 4's goal runner *and* Stage 6's perception before it means anything.

## Control model: vision-grounded, typed-executed

**Decided.** Vision does the finding. UI Automation does the acting. Revia never synthesises a mouse click at a coordinate.

A request resolves in three steps:

1. **Locate.** Qwen3-VL identifies the target semantically from a screen capture — "the Save button, upper left" — including in applications that were never profiled in advance.
2. **Resolve.** That region is matched back to a real UIA element by bounding-box intersection and name agreement, producing a typed element reference.
3. **Act.** The element is invoked through its control pattern, under the existing capability policy, confirmation, dispatch, and audit path.

**If step 2 finds no element, the action is refused.** There is no coordinate fallback. A refusal is a correct outcome, and Revia reports which application and region could not be resolved so the gap is visible rather than silently worked around.

### Why not general coordinate control

Synthetic input at arbitrary coordinates covers every application that renders pixels, which is a real advantage. It was rejected for three reasons specific to this project:

- **It fails open.** Typed control that cannot find its target does nothing and says so. Coordinate control that misjudges by forty pixels clicks Delete instead of Save and discovers it afterwards. Revia is assistive technology for users with limited standard PC interaction, who are disproportionately less able to notice and reverse a wrong action quickly. Failing closed has to be a structural property, not a preference.
- **It breaks Stage 4.** The goal runner verifies outcomes and retries within a budget. Verification requires knowing what was attempted. "Invoked Save" can be checked against "the title bar asterisk cleared." "Clicked (842, 391)" cannot be checked against anything. Coordinate control does not merely weaken the audit log; it removes the precondition for the stage named as the next implementation slice.
- **Its generality is already capped.** UIPI prevents a non-elevated process from sending input to an elevated window, and Stage 5 commits to running under a non-administrator account. The "works everywhere" promise does not survive contact with that constraint anyway.

### Accepted cost

Applications without a usable UIA tree cannot be driven: games, custom-drawn canvas software, some Electron applications, Java without the access bridge, and — relevant to this project's own shell — Qt applications, whose UIA support is partial.

The correct response is to report the limitation, not to reach for coordinates. Where an unreachable application matters enough, the route is an app-native integration (COM, CLI, or file-format manipulation), which is the same model the typed filesystem actions already use.

Revisiting this decision affects Stage 3 and Stage 6's action surface only. The perception, presence, and goal-runner stages are independent of it.

## Stage 1 — Trusted action foundation (implemented)

- Typed action contracts and dispatcher.
- Approved-root and risk policy.
- Supervised and approved-scope execution modes.
- Safe filesystem operations, limits, dry runs, Recycle Bin deletion, and audit log.
- Direct commands and constrained one-action LLM proposals.
- Priority-ordered conversation and background memory agents for each turn.
- Hybrid SQLite FTS5 plus persistent semantic-vector memory, automatic JSONL migration, and background embedding backfill.
- Owned llama.cpp child processes placed in a Windows kill-on-close job.
- Automated policy/executor tests.

## Stage 2 — Desktop shell and presence (implemented)

- Implemented: a separate Qt desktop module with a translucent companion window, tray controls, chat panel, always-on-top option, and visible states for offline, starting, idle, thinking, responding, remembering, acting, waiting, blocked, error, and stopping.
- Implemented: a thread-safe runtime event bus, reusable non-CLI session, timestamped activity/timing feed, cooperative stop button, confirmation dialogs, and clean owned-process shutdown. The CLI remains available as a fallback.
- Implemented: bounded response-posture/affect events, profile-aware Qwen3-TTS voice design/cloning with SAPI fallback, and toggled (Ctrl+Space) CUDA whisper.cpp STT with component telemetry.
- Implemented: horizontal status/actions and tabbed Chat, Activity, Voice Studio, and Settings presentation without moving runtime ownership into Qt.
- Remaining shell work: persist window preferences and add a focused audit-history view instead of mixing audit records with general activity.

**The Qt window is transitional.** It exists so a human can watch what the runtime is doing and chat with it during development. The intended long-term interface is the embodied 3D character described in the Embodiment track, at which point this window is demoted to a debug and inspection surface rather than the primary experience.

That demotion is already affordable because of two decisions made here: runtime ownership never moved into Qt, and `ReviaSession` is usable without any UI. Keep it that way. Any feature that only works when the Qt window is open is a regression against the embodiment track.

Exit criteria: Revia can remain on the desktop without stealing focus, the user can immediately pause it, and UI crashes cannot bypass the action policy.

## Stage 3 — Windows application control (first safe slice implemented)

- Implemented: Microsoft UI Automation inspection, focus, Value-pattern text setting, and Invoke-pattern control activation.
- Implemented: deny-by-default executable allowlist, supervised confirmation, redacted audit fields, parser tests, and runtime telemetry.
- Implemented: opt-in local Qwen3-VL screen capture/analysis as a separate perception path, not coordinate-click authority.
- Implemented: **vision-to-typed UIA resolution.** The desktop's `Use screen` path pins the
  foreground executable from Windows, captures only that foreground window with its real
  screen-space origin, and discards the capture if focus changes mid-frame. It rejects
  unapproved applications before model/UIA work, parses only one bounded invoke/value
  intent, and matches its region against enabled UIA elements using geometry plus
  accessible-name agreement. Low-confidence and ambiguous matches refuse, with no
  coordinate fallback.
- Implemented: execution rechecks the resolved runtime id, name, automation id, and control
  type after confirmation. Resolution evidence and component scores are written into the
  ordinary action audit entry. A live Win32 fixture proves the exact element invokes and a
  changed element refuses.
- Implemented: every approved application now requires a per-executable control scope.
  Mutable Focus/Value/Invoke actions share a configurable rolling per-minute budget and
  minimum interval; rate refusals are blocked policy outcomes and audit-visible.
- Implemented: disposable Explorer windows and eligible Notepad windows are accepted only
  when their HWNDs are distinct from every pre-existing user window, retargeted into a
  separate rehearsal runtime, inspected through UI Automation, and closed by RAII. Modern
  Notepad is additionally rejected when it restores more than one prior document tab; on
  that Windows setting, Revia refuses Notepad rehearsal instead of reading a user session.
  The default profile uses exact observed control names rather than compatibility wildcards.
- Implemented: the Permissions tab inventories enabled foreground Invoke/Value controls
  without granting them, lists all active app/control scopes, and can atomically add or
  revoke a scope with immediate policy reload. Editable permissions live under
  `RuntimeData` so rebuilding cannot silently restore a revoked grant.
- Require confirmation before authentication, purchases, publishing, sending messages, privilege elevation, security-setting changes, or irreversible operations.

Exit criteria: each supported application has deterministic integration tests and a deny-by-default capability profile, and every vision-resolved action either produces a typed element reference or is refused with the reason recorded.

## Stage 4 — Goal runner and self-testing (engine implemented and wired)

- Implemented: a goal persisted as a state machine — validate, act, observe, verify, retry within a bound, then complete or stop — over the existing typed actions. The runner adds no execution authority; every step goes through the same dispatcher, policy, and audit path as an interactive action.
- Implemented: per-step evidence in SQLite (action IDs joinable against the JSONL audit log, observations, expected vs actual, retry count, verdict, final status), plus a per-goal capability scope that cannot widen the profile's authority.
- Implemented: budgets for actions, retries per step, total retries, tokens, and duration; a plan is rejected before execution when a step has no read-only verification action or never says what success looks like.
- Implemented: `ReviaSession` owns the store and runner, republishes step transitions on the runtime event bus as `Goal` component status, routes step confirmations to the same handler interactive actions use, and reports unfinished goals at startup. `/goals` lists them, `/goals resume <id>` continues one.
- Implemented: **goal authoring.** `/goal <request>` asks the local model for a multi-step plan — each step an action, a read-only check, and a literal expectation — decodes it with `GoalPlanner`, refuses more than twelve steps, then runs it through `GoalRunner::Validate` before anything executes. The plan is shown in full and approved as a whole before the first step, and every step still hits the per-action confirmation path.
- Implemented: the plan never names its own capability scope. `NarrowScopeForGoal` derives it from configured policy and can only tighten — mode forced to `ApprovedScope`, root creation refused, auto-approval capped at `ReversibleWrite`.
- Implemented: **the disposable sandbox.** Every `/goal` run is rehearsed first. `GoalSandbox` stages only the paths the plan names into a scratch tree, rewrites the plan onto it, and runs it there. Explorer and isolation-safe Notepad steps additionally launch distinct disposable windows and retarget requests to their exact titles; session-restoring Notepad and other desktop applications fail rehearsal rather than inspecting a user window. A plan that fails rehearsal is refused and never reaches real folders or user windows; a plan that passes is offered for approval *with that evidence attached*. Scratch state and owned fixture windows are removed by scope guards.
- Remaining: the CLI (`reviaApp`) has none of the goal commands; they exist on `ReviaSession` only.
- Remaining: each application beyond Notepad and Explorer needs its own explicit disposable
  fixture and control-level verification before goal rehearsal may support it.

**A rehearsal needs its own `ActionRuntime`, not the session's.** Scoped execution takes the more restrictive of the global policy and the goal's, and the scratch tree sits outside every configured approved root — so running a rehearsal through the session's runtime blocks every step of a perfectly workable plan. `GoalSandbox::Prepare` therefore emits a capability config approving the scratch tree and nothing else, with no approved applications and the goal's own risk ceiling copied across so the rehearsal is never more permissive than the run it stands in for. Its audit log is discarded with the tree rather than mixed into the real trail.

Only the paths a plan names are staged, one directory level deep. Mirroring an entire approved root to rehearse a two-step plan would cost more than the plan does, and a step that reaches outside what it declared fails in rehearsal instead of succeeding there and surprising someone later.

**Asked something vague, the planner proposes deletion.** "Clean up my desktop" produced a single step recycling the whole Desktop directory. Nothing upstream flags it: the plan is structurally valid, and `move_to_recycle_bin` is classified `ReversibleWrite` because the bin is recoverable, so the risk ceiling does not catch it either. What contains it is the approved-root boundary and the fact that the whole plan is shown before it runs — which is why the approval prompt marks a recycling step `[DELETES FILES]` explicitly. Treat that pairing as load-bearing, not cosmetic.
- Implemented: **reviewed learning.** `LearningReview` draws lessons from recorded outcomes — goal stop reasons and proposal accept/dismiss counts — and `/review` offers them with their evidence. Approving one writes an ordinary preference memory through the ordinary memory path; it cannot change a capability, a budget, or a policy. Inference is deliberately kept away from anything that grants authority: the one automatic adjustment, halving the initiative rate below a precision floor, is computed from counted outcomes inside `AttentionPolicy` rather than proposed by a review. A pattern needs at least four samples, unfinished goals are not outcomes, and the review states the unwelcome conclusion when the numbers support it.

Unfinished goals are reported at startup, never auto-resumed. Restarting into unattended execution of work the user has not re-approved is Stage 5, behind its own job contract.

Exit criteria: Revia proves completion through observable checks, stops on repeated failure, and resumes safely after restart.

## Stage 5 — Unattended approved jobs

- Add a scheduler/job queue whose job definition pins the goal, capability profile, approved roots/apps, budgets, and success checks.
- Run under a non-administrator Windows account with the minimum filesystem and application access needed.
- Require leases and heartbeats so stale jobs stop, plus a global kill switch.
- Produce a completion report and retain an audit trail that a human can review afterward.
- Promote one workflow at a time from supervised to unattended only after repeatable sandbox and real-world validation.

Exit criteria: a narrow job can run unattended and fail safely. This is the realistic route to working alone; broad human-equivalent control remains a collection of explicitly approved capabilities rather than one unlimited permission.

## Stage 6 — Ambient perception

*Depends on Stage 3. Independent of Stages 4 and 5.*

"Revia sees you working on something and helps" requires continuous observation. Stage 3 deliberately provides the opposite: capture is opt-in, confirmed per request, and deleted afterwards. That is correct for a user-invoked vision query and useless as an ambient sense.

This stage ports the two-brain design already worked out for AccessMind: a cheap layer that runs constantly and an expensive layer woken only when the cheap layer finds something worth reasoning about. Running Qwen3-VL continuously is not viable at any frame rate; running it a few times an hour on justified evidence is.

**Perception tiers, cheapest first.** Each tier only escalates to the next when it finds a reason to.

- **Tier 0 — window and focus events. Implemented.** `SetWinEventHook` for foreground changes, window creation, and title changes, on a dedicated thread with its own message pump so perception never depends on the Qt window being open. The capability default remains off; the checked-in local profile is enabled at the user's request for context-driven conversation. Deny lists suppress rather than redact; observations are debounced from the last admitted one and capped by a rolling per-minute budget; only counts are logged, never titles. Measured on a real desktop: 22 raw events produced 7 observations and 15 coalesced, and a window whose title matched the deny list was counted as excluded with nothing about it written anywhere. **Resist escalating to Tier 1 until this is exhausted** — most of what the later tiers would ask a model to infer is already available here for free.
- **Tier 1 — change detection.** Periodic BitBlt capture, downscaled, reduced to a perceptual hash. Discard frames that are materially identical to the last. Bounded to a fixed interval with a hard cap. `gdi32` and `gdiplus` are already linked.
- **Tier 2 — cheap local analysis.** OCR or a small classifier over changed regions only. Produces structured observations, not prose.
- **Tier 3 — reasoning.** Wake Qwen3-VL only when lower tiers produce a salience score above threshold, with a hard budget of wakes per hour. Every wake must record what evidence justified it, so false wakes are diagnosable.

**Attention and interruption model.** This is the part that determines whether the feature is delightful or intolerable, and it is not a model prompt — it is deterministic policy.

- Silence is the default. Revia must clear a confidence threshold to speak, not a relevance one.
- Cooldown after every utterance, and a longer cooldown after a dismissal.
- Hard suppression during full-screen applications, screen sharing, active typing bursts, and any application on the perception exclusion list.
- Every proactive utterance is dismissible in one action, and dismissals are recorded.
- Track precision: proposals accepted versus dismissed. Below a configured ratio, Revia reduces its own rate automatically. A proactive assistant that cannot tell it is being annoying is a defect.

**Privacy.** Continuous screen observation is the most invasive capability in this project and must be treated as more sensitive than filesystem writes.

- Off at the capability default, with explicit opt-in separate from Stage 3's per-capture consent. The repository's current settings opt in because the user requested event-driven conversation starters.
- A persistent, unmissable indicator while observation is active.
- An exclusion list by executable and window title, defaulting to deny for password managers, banking, and private browsing.
- Frames are analysed and discarded; only structured observations persist, and those follow the existing memory sensitivity rules.
- A single global pause that stops perception without stopping Revia.

**Session history (implemented).** Tier 0 observations roll into bounded in-memory spans: consecutive observations of one application merge, and a span runs until the *next* application appears rather than until its own last window event, because a quiet application generates no events and reporting no time for forty minutes of reading answers the question wrongly. That attribution is capped at five minutes so an idle machine does not credit hours to whatever was last in front. Capped at 240 spans, eight hours, and four titles per span; never written to disk; cleared by `/perception forget` and discarded when Revia stops.

Exit criteria: Revia can describe what the user has been doing for the last hour from Tier 0 and Tier 1 evidence alone, with Tier 3 wakes staying inside budget, and observation can be paused instantly and verifiably.

The first half of that is met from Tier 0 alone — `/perception history 60` reports time per application with the files worked on — and pause is immediate. Tier 1 and above remain unbuilt. Tier 0 now feeds only bounded event patterns into the Stage 7 attention gate; it still cannot speak or wake a model directly.

## Stage 7 — Self-directed goal formation (first slice implemented)

- Implemented: `InitiativeController` forms proposals from Tier 0 session evidence and offers them through `AttentionPolicy`, a deterministic gate. A proposal carries its evidence, executes nothing, and is dismissible in one action. Accepting one that names a goal hands it to the Stage 4 runner under the existing rehearsal, confirmation, budget, and audit path — this stage adds no execution capability whatsoever, which is what makes it safe to build before the delivery model matures.
- Implemented: confidence threshold rather than a relevance one; cooldown after every utterance and a longer one after dismissal; hourly ceiling; hard suppression during full-screen applications, excluded applications, and active input; refusal to repeat an observation; dismissal recorded; precision tracked and the hourly rate halved automatically below the configured ratio, recovering when proposals land.
- Implemented: the decision to speak is policy, not a prompt. The model supplies content and a confidence; when it is welcome to interrupt is decided here, for the same reason it does not choose its own capability scope.
- Implemented: **evidence sources are ranked by how concrete they are.** An unfinished goal outranks a session observation, because a goal is something the user actually asked for while time spent in an editor is only an observation about it. Accepting a goal-backed proposal hands it straight to `ResumeGoal`, which re-verifies every remaining step: accepting is a shortcut for typing the command, never a way around it.
- Implemented: **event-driven conversation openings.** A completed focus stretch, returning to an application after an absence, repeated movement between two applications, or an unfinished goal is the cause. A condition-variable worker sleeps indefinitely without one of those signals. Quiet-input delay and cooldown only debounce or suppress an event; they never manufacture one. Conversation cues are one-shot and expire instead of resurfacing when a timer ends.
- Implemented: ordinary openings are generated through `ConversationRuntime`, enter dialogue history as Revia's line, and continue from a natural user reply. “Not now” and similar natural refusals record a dismissal; slash commands remain only for action-backed proposals.
- Remaining: further evidence sources — a file edited repeatedly by hand that a goal could do, an approved root filling up. Each needs no new authority, and the precision counter is the honest test of whether one earns its place.
- Remaining: report proposal precision to the user prominently rather than only through `/initiative`.



*Depends on Stage 4 and Stage 6.*

Stage 4 executes a goal. It does not decide one. Every stage before this assumes the user supplied the intent, which is the line between an agent and an autonomous companion — currently uncrossed.

- Revia emits **proposals, never actions**. A proposal carries the observed evidence, the goal it would pursue, the capability profile it would need, and the budget it would consume. It executes nothing.
- The user approves **intent, not each action**. Once approved, the proposal becomes a Stage 4 goal and runs under the existing policy, dispatcher, and audit path with no new authority whatsoever. This stage adds no execution capability — that property is what makes it safe to build.
- Proposals are rate-limited and budgeted like any other resource.
- Dismissal is signal. Store it as reviewed memory and measured policy adjustment, consistent with Stage 4's rule that learning is not self-modifying executable code.
- Report proposal precision to the user. If Revia is wrong most of the time, that must be visible rather than inferred from irritation.

Exit criteria: over a working week, Revia's accepted proposals exceed its dismissed ones, and no proposal has ever executed without approval.

## Embodiment track — desktop presence

*Depends only on Stage 2's event bus. Fully parallel; touches no policy code.*

The target is a character occupying the desktop itself rather than a window on it — closer to Desktop Mate than to a chat client. This is almost entirely independent of the AI work, which makes it the safest track to develop in parallel and the easiest to hand off.

- **Overlay window.** `WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE`, per-pixel alpha via `UpdateLayeredWindow`. Absent from the taskbar and Alt-Tab. Must never take focus — this is the same constraint as Stage 2's exit criteria and it is non-negotiable, because a companion that steals focus while the user types is unusable.
- **Selective hit-testing.** Click-through everywhere except the character's own silhouette, so the desktop underneath stays fully usable.
- **Window awareness.** Enumerate top-level windows to sit on edges, perch on title bars, and get out of the way of the foreground window.
- **Direct manipulation.** Drag, drop, flick, and place. Position persists across restarts.
- **State-driven animation** from the *same* runtime event bus Stage 2 already publishes — idle, thinking, responding, acting, blocked, error. The renderer subscribes to events; it never queries the runtime and never gates it.
- **Idle behaviour.** The character remains alive between interactions. This is presentation only and must not wake the perception or reasoning tiers.
- **Isolation.** Rendering runs in its own process, or at absolute minimum its own thread with no runtime ownership. A GPU driver fault in the avatar must not take down inference, memory, or the action policy. Stage 2 already states this principle; embodiment is where it gets tested for real.
- **3D.** Qt Quick 3D is the low-friction path given the existing Qt dependency. VRM is the format this category of application has standardised on and is worth targeting for avatar interchange.

Exit criteria: the character runs for a full day without stealing focus or dropping input to the applications beneath it, and killing the renderer leaves the runtime unaffected.

## Suggested next implementation slice

Stage 4 executes goals, Stage 6 Tier 0 observes and summarises a session, and Stage 7's first slice now connects them: Revia can offer something unprompted, behind a deterministic attention gate, and be interrupted mid-sentence.

The vision-to-typed resolver, per-control policy, desktop rate budgets, and disposable
application safety gate are connected. Explorer is live-fixture validated; Notepad runs
only when Windows supplies a single isolated document window and otherwise reports a safe
skip. The next Stage 3 slice
is **one complete useful application adapter**: choose a narrow workflow, pin its stable
automation ids through the Permissions inventory, exercise read/value/invoke success and
stale-element refusal, and add its own disposable fixture. Do not add broad application
access or a compatibility wildcard.

Bounded internet grounding and live permission editing are now implemented as separate
single-purpose capabilities. The next quality slice is an evaluation corpus that runs the
active local model against the conversation contract and records pass/fail alongside the
runtime quality counters; deterministic heuristics can flag regressions but cannot prove
that sampling still sounds natural.

The `InputArbiter` is now wired into `Submit`: typed input is preserved exactly, while final voice transcripts pass through noise and duplicate admission before they become turns. Always-on VAD listening still has settings but no continuous capture loop; push-to-talk/toggle recording remains the enabled path because a standing microphone needs its own explicit privacy control.

Measure before adding evidence sources. The precision counter is the honest signal for whether a new source earns its place, and a source that lowers precision should be removed rather than tuned.

**Tier 1 perception** stays deferred. Change detection and perceptual hashing add cost and privacy surface, and most of what they would infer is already available for free from window titles.

**Reviewed learning** (Stage 4's last bullet) remains open but still has no consumer: nothing produces enough goal history for a review loop to act on.
