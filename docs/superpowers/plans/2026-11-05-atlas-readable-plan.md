# Atlas readable progress Implementation Plan

> For agentic workers: use executing-plans to implement this plan task by task.

Goal: Make Atlas an accurate, readable project entry point using the portfolio
as the visual reference.

Architecture: Keep two static HTML pages and exactly the existing six public
files. A dependency-free Node generator preserves full historical HTML/JSON
inside clearly dated records and builds current summaries from curated data.

Tech stack: HTML, CSS, vanilla JavaScript, Node built-in tests.

Spec: ../specs/2026-11-05-atlas-readable-design.md

## Global constraints

- Source pin d51a2b38; installed application unverified; all work paused.
- Current CI155 passed,0 failed,2 optional skips,6 successful jobs.
- RNL local studies011–016;017 proposed; comparison incomplete; no integration.
- Preserve historical detail and links; no publication or product execution.
- Exactly six public files, combined size below500000 bytes; original privacy guard.

## Review focus

- Stale historical claims must be visibly dated and subordinate to current status.
- Local studies must never acquire public evidence links or finished-comparison claims.
- A deep link into closed historical details must expose its target.
- Mobile navigation must release focus and restore access after close/resize.
- Search must expose matches inside closed records and recover after clearing.

## Task1: Accurate current snapshot and source preservation

- [x] Audit GitHub source, portfolio, stopping handoffs and Pages evidence.
- [x] Build in an independent checkout on codex/atlas-readable-handoff.
- [x] Add a content/byte-boundary verification that fails on the stale current snapshot.
- [x] Implement curated current data, generator and dated record preservation.
- [x] Verify exact six-file boundary, privacy pattern, totals and source identities.

## Task2: Readable visual site and navigation

- [x] Implement portfolio-informed CSS, semantic HTML and optional technical detail.
- [x] Implement mobile menu, query filtering, clear and historical deep links.
- [x] Check real desktop/mobile views, keyboard navigation, filtering and no results.
- [x] Check console errors, links, overflow, no-JavaScript structure and print styles.

## Task3: Handoff and independent review

- [x] Add docs/AI_HANDOFF.md and link the current entry point from README.
- [x] Preserve source audit, Pages diagnosis and validation results outside public files.
- [x] Run a fresh independent content/code review and address material findings.
- [x] Verify final diff contains documentation/Atlas only; report local-only outcome.
