# Stage B image continuity implementation plan

> **For agentic workers:** Use the existing owners and verification workflow. The user authorized completing this stage autonomously; preserve the current checkout and leave all work unstaged and uncommitted.

**Goal:** Make private image upload, grounded reply, selective pixel reinspection, multi-image comparison and supported replay/restoration work through the existing local model path.

**Architecture:** Retain immutable attachment IDs and content digests on the original conversation message and archived turn, without retaining pixel buffers in history. Resolve only the selected references through the companion's existing attachment library, validating its current record, owned path, byte limit and content digest on every read. Existing audience, participant, conversation, admission and cancellation owners remain authoritative.

**Tech stack:** C++/Qt, existing SQLite attachment and conversation stores, llama.cpp and the installed local vision model/projector.

**Spec:** User's attached master request, sections 6A/6B and Stage B, and `docs/COMPANION_UPGRADE_PROGRESS.md`.

## Constraints and review focus

- Preserve Stage A changes, personal data, configuration preferences and existing generation.
- No cloud fallback, unapproved model downloads or unreviewed dependencies.
- Use isolated roots for live checks; no success claims from transport fixtures alone.
- Refuse stale, forgotten, tampered or wrong-scope references and invalidate pending publication.
- Ambiguous image identity requires clarification; unrelated turns carry no pixels.
- Edit/Regenerate/Retry preserve valid associations while replacing obsolete answers.
- Desktop intake captures its originating session and joins before session retirement.
- Reference editing must never fall through to unrelated text-to-image generation.

## Work

- [x] Reproduce current upload-only pixel behavior and measure installed vision/projector pairs in isolation; retain failed semantic results.
- [x] Add shared value references and attachment reinspection with bounded reads and regression tests.
- [x] Retain message associations in private history/archive; select references for follow-up, replay and restoration with scope and forgetting tests.
- [x] Repair Desktop intake lifetime and preserve attach/paste/drop/removal/preview behavior with focused checks.
- [x] Run actual content-based upload, image-only, follow-up, multi-image, replay and restoration acceptance, including unavailable/corrupt/forgotten/hostile media cases.
- [x] Retest installed text-to-image generation; verify readable output and reference-edit refusal.
- [x] Build final affected targets and pass the 14 selected CTest suites, with independent source and test review.
- [x] Build affected targets, run appropriate existing checks and independent review, verify cleanup, and record commands/evidence/limits in progress notes.

The chosen approach extends current message and store owners. Sending all historical images would violate the requested bounded selective access; keeping only model descriptions would not support actual pixel reinspection.
