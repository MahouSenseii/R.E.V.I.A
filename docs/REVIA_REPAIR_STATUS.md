# Revia repair status

Baseline: `ad207d6f04aad2a6981fa2a26a8f35124f287c0f`. Audited base: `44aa6bc4d890fe050a4336c530b340d08a3a0767`.
Candidate worktree: `C:/Users/davis/.codex/worktrees/revia-audit-repairs/R.E.V.I.A`, branch `codex/revia-audit-repairs`.
No existing repair candidate/handoff was found in local/remote branches, active/archived worktree records, or repository docs before starting.

Plan: [audit repairs](superpowers/plans/2026-10-09-audit-repairs.md).

| Area | Owner | State | Next evidence |
| --- | --- | --- | --- |
| R2–R4 policy/schema/continuity | policy worker, root integration | Independent review follow-ups repaired; fresh combined production wire suite passes | Full native inventory and paired evaluation |
| R5/R11 provenance/desktop | scope worker | Sixteen scope controls pass; desktop fixture diagnosis and correction pass | Full native inventory and real-model worker outcomes |
| R1/R7/R10 build/Windows/CLI | build worker | Measured build remedy; driver/operator controls and fresh CLI-only positive/negative pass | Complete native build/CTest and hosted CI |
| R6 journal | root | Fresh read and post-write health red/green; independent journal suite passes | Integrated suite and Linux CI |
| R8/R9 integration/review/handoff | root | Branch assessment retained; independent reviewer active | Frozen candidate, paired evaluation and completion handoff |

## Decisions

- The user's latest implementation instruction authorizes proceeding with the supplied engineering brief; no repeated design approval is required.
- The original attached R1–R9 prompt plus independent review and explicit R10/R11 instruction are the available brief. No separate newer prompt file was found.
- Production memories/models/settings remain in the primary checkout. All repair fixtures/builds use isolated paths.

## Evidence ledger

Initial tracked primary checkout was clean. Local and remote main both resolved to the baseline above. Existing R8 remote branch remains `0b93facf5920cd80721fa9a3929ad6b556201c7f` with 40 main-only/31 branch-only commits at baseline.

- R6: actual production read failure reproduced after adding a stream-source seam, before changing the reader. Checked chunked reads pass initial/mid-read failure, missing/empty/valid/denied/directory and recovery controls. Fresh unchanged durability/scope/crash/repair suite passes. Evidence: `build/repair-evidence/journal/report.txt` and red/green receipts.
- R5: scope worker also owns narrowly scoped `GoalTokenScope` admission/lifetime blocks in `Private/Runtime/reviaSession.cpp`, because admitted goal actions need their real task context rather than a global unknown-task fallback. Existing goal/cancellation controls must stay passing.
- R1: fresh full-symbol Foundation build completed in 824.19 seconds. Archive phase took 329.09 seconds, output 922.85 MB, measured peak working set 2.34 GB/private 2.42 GB. This moving-source pre-integration build is a resource measurement, not the frozen baseline/candidate qualification.
- R9: separate baseline worktree `C:/Users/davis/.codex/worktrees/revia-repair-baseline/R.E.V.I.A` retains baseline production. Both sides will receive the identical recorded developer-tool evaluation overlay. A 13-design, two-seed cohort was authored before outcomes; frozen source replay tests claim support, not live browser autonomy. Seeded continuity runs use actual bounded history/archive restoration and a real final model answer. Background live qualification is separately inventoried.
