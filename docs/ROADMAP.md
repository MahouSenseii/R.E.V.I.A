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
- Remaining: add per-control/rate limits and deterministic app integration fixtures before enabling more applications.
- Remaining: **wire the vision path to the typed path.** Both halves exist and are not connected. Add a resolver that takes a vision-identified screen region and returns a typed UIA element reference, matching by bounding-box intersection and name agreement, with a confidence threshold below which it refuses. This is what turns the allowlist from "applications someone profiled by hand" into "applications that expose a UIA tree," which is a far larger set, without adding any new execution authority.
- Remaining: record the resolution evidence in the audit entry — what the model identified, which element it resolved to, and the match confidence — so a wrong resolution is diagnosable after the fact rather than only observable in its consequences.
- Require confirmation before authentication, purchases, publishing, sending messages, privilege elevation, security-setting changes, or irreversible operations.

Exit criteria: each supported application has deterministic integration tests and a deny-by-default capability profile, and every vision-resolved action either produces a typed element reference or is refused with the reason recorded.

## Stage 4 — Goal runner and self-testing (engine implemented and wired)

- Implemented: a goal persisted as a state machine — validate, act, observe, verify, retry within a bound, then complete or stop — over the existing typed actions. The runner adds no execution authority; every step goes through the same dispatcher, policy, and audit path as an interactive action.
- Implemented: per-step evidence in SQLite (action IDs joinable against the JSONL audit log, observations, expected vs actual, retry count, verdict, final status), plus a per-goal capability scope that cannot widen the profile's authority.
- Implemented: budgets for actions, retries per step, total retries, tokens, and duration; a plan is rejected before execution when a step has no read-only verification action or never says what success looks like.
- Implemented: `ReviaSession` owns the store and runner, republishes step transitions on the runtime event bus as `Goal` component status, routes step confirmations to the same handler interactive actions use, and reports unfinished goals at startup. `/goals` lists them, `/goals resume <id>` continues one.
- Implemented: **goal authoring.** `/goal <request>` asks the local model for a multi-step plan — each step an action, a read-only check, and a literal expectation — decodes it with `GoalPlanner`, refuses more than twelve steps, then runs it through `GoalRunner::Validate` before anything executes. The plan is shown in full and approved as a whole before the first step, and every step still hits the per-action confirmation path.
- Implemented: the plan never names its own capability scope. `NarrowScopeForGoal` derives it from configured policy and can only tighten — mode forced to `ApprovedScope`, root creation refused, auto-approval capped at `ReversibleWrite`.
- Implemented: **the disposable sandbox.** Every `/goal` run is rehearsed first. `GoalSandbox` stages only the paths the plan names into a scratch tree, rewrites the plan onto it, and runs it there. A plan that fails rehearsal is refused and never reaches real folders; a plan that passes is offered for approval *with that evidence attached*, so the user approves an observed result rather than plausible-looking text. The scratch tree is removed by a scope guard, so an exception cannot leave one behind.
- Remaining: the CLI (`reviaApp`) has none of the goal commands; they exist on `ReviaSession` only.
- Remaining: rehearsal is filesystem-only. A plan that drives an application has nothing to copy, so it reports that it could not be rehearsed and goes to confirmation without evidence. Rehearsing UIA actions needs a disposable application instance, not a disposable directory.

**A rehearsal needs its own `ActionRuntime`, not the session's.** Scoped execution takes the more restrictive of the global policy and the goal's, and the scratch tree sits outside every configured approved root — so running a rehearsal through the session's runtime blocks every step of a perfectly workable plan. `GoalSandbox::Prepare` therefore emits a capability config approving the scratch tree and nothing else, with no approved applications and the goal's own risk ceiling copied across so the rehearsal is never more permissive than the run it stands in for. Its audit log is discarded with the tree rather than mixed into the real trail.

Only the paths a plan names are staged, one directory level deep. Mirroring an entire approved root to rehearse a two-step plan would cost more than the plan does, and a step that reaches outside what it declared fails in rehearsal instead of succeeding there and surprising someone later.

**Asked something vague, the planner proposes deletion.** "Clean up my desktop" produced a single step recycling the whole Desktop directory. Nothing upstream flags it: the plan is structurally valid, and `move_to_recycle_bin` is classified `ReversibleWrite` because the bin is recoverable, so the risk ceiling does not catch it either. What contains it is the approved-root boundary and the fact that the whole plan is shown before it runs — which is why the approval prompt marks a recycling step `[DELETES FILES]` explicitly. Treat that pairing as load-bearing, not cosmetic.
- Remaining: treat learning as reviewed memory and measured policy updates—not self-modifying executable code.

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

- **Tier 0 — window and focus events. Implemented.** `SetWinEventHook` for foreground changes, window creation, and title changes, on a dedicated thread with its own message pump so perception never depends on the Qt window being open. Off by default; deny lists suppress rather than redact; observations are debounced from the last admitted one and capped by a rolling per-minute budget; only counts are logged, never titles. Measured on a real desktop: 22 raw events produced 7 observations and 15 coalesced, and a window whose title matched the deny list was counted as excluded with nothing about it written anywhere. **Resist escalating to Tier 1 until this is exhausted** — most of what the later tiers would ask a model to infer is already available here for free.
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

- Off by default, with explicit opt-in separate from Stage 3's per-capture consent.
- A persistent, unmissable indicator while observation is active.
- An exclusion list by executable and window title, defaulting to deny for password managers, banking, and private browsing.
- Frames are analysed and discarded; only structured observations persist, and those follow the existing memory sensitivity rules.
- A single global pause that stops perception without stopping Revia.

**Session history (implemented).** Tier 0 observations roll into bounded in-memory spans: consecutive observations of one application merge, and a span runs until the *next* application appears rather than until its own last window event, because a quiet application generates no events and reporting no time for forty minutes of reading answers the question wrongly. That attribution is capped at five minutes so an idle machine does not credit hours to whatever was last in front. Capped at 240 spans, eight hours, and four titles per span; never written to disk; cleared by `/perception forget` and discarded when Revia stops.

Exit criteria: Revia can describe what the user has been doing for the last hour from Tier 0 and Tier 1 evidence alone, with Tier 3 wakes staying inside budget, and observation can be paused instantly and verifiably.

The first half of that is met from Tier 0 alone — `/perception history 60` reports time per application with the files worked on — and pause is immediate. Tier 1 and above remain unbuilt, and the attention/interruption model below applies to none of it yet, because nothing here speaks: this stage only answers when asked.

## Stage 7 — Self-directed goal formation

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

Stage 4 can execute a goal. Stage 6 Tier 0 can now observe and summarise a working session. The two have never been connected, and connecting them is Stage 7 — the stage that turns a capable assistant into a companion.

**A proposal path** is therefore the next slice, and it is smaller than it sounds because it adds no execution authority whatsoever. A proposal carries the observed evidence (from the session history that now exists), the goal it would pursue, the capability profile it would need, and the budget it would consume. It executes nothing. Approving one hands it to the Stage 4 runner, which already rehearses, confirms, budgets, verifies, and audits. That property — that this stage cannot do anything the user has not already been able to do by typing `/goal` — is what makes it safe to build before the attention model exists.

Deliberately out of scope for that first slice: Revia speaking unprompted. Proposals should be *retrievable* before they are *delivered*. The Stage 6 attention and interruption rules — cooldowns, suppression, dismissal tracking, precision measurement — all belong to delivery, and building them before there is anything worth delivering gets the order backwards.

**Tier 1 perception** stays deferred. Change detection and perceptual hashing add cost and privacy surface, and most of what they would infer is already available for free from window titles.

**Reviewed learning** (Stage 4's last bullet) remains open but still has no consumer: nothing produces enough goal history for a review loop to act on.
