# Roadmap to the desktop-companion vision

The goal is a Revia that can live on the Windows desktop, converse naturally, see and operate supported applications, pursue bounded goals, test its work, and eventually handle pre-approved work without constant oversight. That requires staged autonomy; it should not be implemented as unrestricted mouse, keyboard, shell, or administrator control.

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
- Implemented: bounded response-posture/affect events, profile-aware Qwen3-TTS voice design/cloning with SAPI fallback, and hold-to-talk CUDA whisper.cpp STT with component telemetry.
- Implemented: horizontal status/actions and tabbed Chat, Activity, Voice Studio, and Settings presentation without moving runtime ownership into Qt.
- Remaining optional presentation work: avatar animation driven by explicit state events; keep rendering independent from reasoning and permissions.
- Remaining shell work: persist window preferences and add a focused audit-history view instead of mixing audit records with general activity.

Exit criteria: Revia can remain on the desktop without stealing focus, the user can immediately pause it, and UI crashes cannot bypass the action policy.

## Stage 3 — Windows application control (first safe slice implemented)

- Implemented: Microsoft UI Automation inspection, focus, Value-pattern text setting, and Invoke-pattern control activation.
- Implemented: deny-by-default executable allowlist, supervised confirmation, redacted audit fields, parser tests, and runtime telemetry.
- Implemented: opt-in local Qwen3-VL screen capture/analysis as a separate perception path, not coordinate-click authority.
- Remaining: add per-control/rate limits and deterministic app integration fixtures before enabling more applications.
- Require confirmation before authentication, purchases, publishing, sending messages, privilege elevation, security-setting changes, or irreversible operations.

Exit criteria: each supported application has deterministic integration tests and a deny-by-default capability profile.

## Stage 4 — Goal runner and self-testing

- Persist a goal as a state machine: plan, preflight, act, observe, verify, retry with a bound, then complete or stop.
- Store evidence for every step: action IDs, observations, expected outcome, actual outcome, retry count, and final status.
- Add budgets for time, tokens, actions, disk changes, and retries.
- Run goals inside a disposable sandbox before allowing them against real folders or applications.
- Treat learning as reviewed memory and measured policy updates—not self-modifying executable code.

Exit criteria: Revia proves completion through observable checks, stops on repeated failure, and resumes safely after restart.

## Stage 5 — Unattended approved jobs

- Add a scheduler/job queue whose job definition pins the goal, capability profile, approved roots/apps, budgets, and success checks.
- Run under a non-administrator Windows account with the minimum filesystem and application access needed.
- Require leases and heartbeats so stale jobs stop, plus a global kill switch.
- Produce a completion report and retain an audit trail that a human can review afterward.
- Promote one workflow at a time from supervised to unattended only after repeatable sandbox and real-world validation.

Exit criteria: a narrow job can run unattended and fail safely. This is the realistic route to working alone; broad human-equivalent control remains a collection of explicitly approved capabilities rather than one unlimited permission.

## Suggested next implementation slice

Build the bounded goal runner: a persisted plan/act/observe/verify state machine with action, time, token, and retry budgets. Start with a disposable Notepad fixture and require observable success evidence before a goal can complete.
