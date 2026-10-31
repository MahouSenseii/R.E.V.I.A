# Companion upgrade continuation

## Cloud branch continuation, 2026-10-31 (not a local checkpoint)

This work is on branch `claude/revia-companion-upgrade-eyhpjs`, started from `8aa4ca5`.
It was done in a cloud Linux container without the Windows checkout, `build/` receipts,
models, GPUs, Qt/MinGW or the desktop. The D5 wrap checkpoint below remains the
authoritative local state: nothing here qualifies Stage D or E, or changes the normal
deployment. Evidence is labeled by where it ran: **Linux fixture** (production sources
compiled with g++ 13.3 on Linux), **Windows CI** (GitHub's runner, not the owner's host),
or **host** (not run).

Inventory at `8aa4ca5`, from tracked code only:

- Main's chat launch passes no `--batch-size`/`--ubatch-size` (only the embedding server
  does) and no speculative-decoding flags.
- Ordinary chat, including code written in chat, samples with DRY 0.8 and allowed length
  2. The self code review and saved-source review paths turn DRY off, and their comment
  records it shortening code lines with "..." and ending replies mid-string. That is a
  lead for the Stage E malformed-code failure, not a result. The prepared exact-request
  pair is still the test.
- The error messages at `Tools/revia_video_worker.py:220,227` are missing spaces
  ("at least45GiB"). The file was left unchanged because it is part of the pending D5 run;
  fix it before that run so the qualification covers the final bytes.
- `Tests/audiencePrivacyTests.cpp:948` still expects the Stage B `Reference-image` refusal.
- Linux fixture: `Tests/reviaVideoWorker.test.py` 7 passed and 1 skipped (it needs the
  Windows resource API); `Tests/reviaImageService.test.py` 26 of 26 passed.

1. **Prompt batch settings (roadmap 6.3).** `llm.batchSize` and `llm.microBatchSize`
   (owners: `endpointSettings`/`configManager`) are validated, reach `--batch-size` and
   `--ubatch-size` only when set, reset to llama.cpp's defaults for Fast and Expert
   (`reviaSession`), and are reported as actually built from the launch's own llama.cpp
   log (`llamaCppServerProcess`). Configured values appear in `/status`. Linux fixture:
   `Tests/promptBatchSettingsTests.cpp` passed, and three deliberate code removals each
   failed it; the existing image-token check still passes. Windows CI: it built and passed
   (run 41156920820, commit `58335fd`). Host: not
   benchmarked, and the default launch is unchanged. Next: the `llama-bench` sweep in
   `docs/CONTEXT_BENCHMARK.md`.
2. **Speculation and Next-Concept Modeling research (roadmap 6.5, 9).** Nothing was
   enabled. The pinned llama.cpp `b10453` supports MTP, DFlash, EAGLE-3 and n-gram
   speculation, including checkpointed speculation for Qwen3.5's hybrid recurrent layers.
   MTP is unavailable with the installed GGUF: GGUF headers read from Hugging Face show
   neither the unsloth nor the LM Studio 9B Q4_K_M kept the base model's one MTP head.
   A first-party `z-lab/Qwen3.5-9B-DFlash` draft exists. Both routes need the 19.3 GB
   official checkpoint, which the owner approved on 2026-10-31 (see item 5). n-gram modes can be
   qualified on the host now. The tracked manifest does not describe the installed LM
   Studio Q4_K_M/BF16 pair. Next-Concept Modeling has only task-specific checkpoints and
   no chat or Qwen-compatible model. Details: `docs/SPECULATIVE_DECODING.md`,
   `docs/NEXT_CONCEPT_MODELING.md`.
3. **Dataset export (roadmap 7).** The new `Learning/datasetExport` owner builds
   deterministic train/validation/test JSONL and a manifest from reviewed candidates.
   The target is either a correction bound to the exact judged failure or a
   criterion-verified output, and a failed answer is only ever the rejected side. Related
   examples are grouped before splitting, earlier exports' partitions are kept, and
   withdrawn sources are excluded at export and again before use. Linux fixture:
   `Tests/datasetExportTests.cpp` passed, and four deliberate code removals each failed
   it. Not yet wired to runtime capture, and no real failure has been exported; the
   remaining steps are in `docs/OFFLINE_TRAINING.md`.
4. **Training worker (roadmap 8).** `Tools/revia_training_worker.py` runs one optional
   BF16 LoRA job. It verifies the base install manifest and the dataset digests, rechecks
   withdrawals before model work and never reads the test partition. It refuses quantized
   LoRA and thinking-mode rendering, supports cancellation with checkpoint and resume, and
   publishes an immutable `candidate.json` that is never promoted by the worker. Linux
   fixture: 11 orchestration tests passed, and three deliberate code removals each failed
   one. The real backend trained a tiny hybrid Qwen3.5 on CPU, was cancelled at a
   checkpoint, resumed, and its adapter changed the base model's output. That run also
   showed Qwen3.5's thinking template breaks naive response masking. A real 9B adapter is
   blocked: about 22 GB is needed for BF16 LoRA against a 12 GB GPU, plus the then-unapproved
   base download, no reviewed data yet, and no GGUF roundtrip. The 4B, offload or
   rented-GPU alternatives are owner decisions.

5. **Approved downloads and installer (2026-10-31).** The owner approved the official
   `Qwen/Qwen3.5-9B` checkpoint and `z-lab/Qwen3.5-9B-DFlash`. They then approved the base
   `Qwen/Qwen3.5-4B` (`851bf6e8`) for the separately evaluated 4B experiment.
   `Config/training_assets.json` pins all 31 files, about 31.3 GB, by size and SHA-256.
   `Tools/install_training_assets.py` installs one snapshot outside OneDrive, resumes byte
   ranges, never replaces a file, and writes the deterministic install manifest the worker
   verifies. Linux fixture: 7 tests, and two deliberate code removals each failed one. A real
   install of two pinned 4B files from huggingface.co matched their pinned digests. Nothing
   has been downloaded to the host.
6. **Adapter conversion and llama.cpp check.** `Tools/revia_adapter_export.py` converts a
   verified base (the matched baseline, keeping a real MTP head) and a candidate (as a GGUF
   LoRA, refused for any other base). It then checks the template, end-of-turn token,
   rendering with tools, tokenization and block count in llama-server. Linux, with
   llama.cpp `b10453` built here: 6 fixture tests passed, and a full tiny Qwen3.5 roundtrip
   (train, convert, LoRA, check) passed every gate. The LoRA changed the server's replies.
   Findings along the way:
   - the converter's pinned transformers 4.57.6 cannot read tokenizers saved by
     transformers 5;
   - converting a declared MTP head that has no weights breaks loading;
   - llama-server `b10453` parses every generation against the template's output format.
7. **4B experiment plan.** `docs/OFFLINE_TRAINING.md` gives the exact install, environments,
   job (pinned base and install digest, BF16 LoRA on attention plus MLP, a 10,500 MiB VRAM
   floor that requires Revia to be stopped) and criteria fixed before the run. The worker now
   records `peakVramMiB`, so a 5-step run answers whether peft fits the 12 GB card. Blocked
   on a reviewed dataset export and an evaluation runner for an owned llama-server.

8. **Evaluation runner (2026-10-31).** `Tools/revia_adapter_evaluation.py` compares a
   candidate with its matched base: the same quantized file served with and without `--lora`
   from owned llama-servers. `plan` freezes the inputs and criteria before anything is
   generated. `run` replays the held-out prompts exactly, judges the tool cases
   (`Config/Evaluation/adapter-tool-cases.json`) mechanically, and drives the existing
   `ReviaAnswerGateLive` and `ReviaAnswerQualityLive` against each arm. `review` writes a
   blinded A/B sheet for the owner, and `score` decides accept, reject or incomplete.
   Linux: 7 fixture tests passed, and three deliberate rule removals each failed one. A smoke
   run against a real llama-server `b10453` on the tiny model ran both arms end to end. It
   found that `check` run results overwrote the base identity and adapter flag; that is fixed
   and asserted. The C++ quality tools have only been driven through stand-ins here. Windows
   CI passed it with the other branch checks (run 41164612553, commit `db6114e`).

9. **Capture from recorded failures (2026-10-31).** `Tools/revia_dataset_capture.py` takes
   either a quality report with its saved observation-proxy traffic, or one exact request and
   response pair. It binds each selected failing turn to the single request whose raw output
   is that reply, keeps the exact messages, refuses image, tool-call and ambiguous turns, and
   writes a corrections template for the owner. `candidates` produces
   `revia-dataset-candidates-v1`. The new `ReviaDatasetExport` reads that file with the
   strict `ReadDatasetCandidates` and exports through the existing `Learning/datasetExport`.
   Linux: 5 tests passed; the end-to-end test ran the real C++ exporter, and the training
   worker read and later refused the result. Three deliberate removals each failed a test, and
   the C++ reader's round-trip test passed. In Windows CI (run 41175961328, commit `4df13d5`)
   the new `ReviaDatasetExport` built, and all 5 tests passed, including the end-to-end case
   against the built exporter. The Stage E text failures can be captured on the host; the
   image-feeling failure cannot, because the training path is text-only.

10. **Live capture from the running companion (2026-10-31).** The owner judges a displayed
    reply and ticks a new, unticked-by-default box in the Review dialog. The session then
    writes the exact exchange behind that reply to
    `RuntimeData/Learning/Captures/live/<id>.json`, carrying the owner's recorded judgment.
    - `llamaCppService` attaches the exact request and raw output to each text reply, and
      the session holds the last 8 private replies in memory only.
    - `Learning/liveCapture` binds the capture to the judged reply's digest. It refuses
      replacements and rewrites, configuration changes, pictures, tool steps, thinking,
      unnamed private reference material and secrets.
    - Deleting the chat, forgetting all chats or forgetting the speaker deletes the capture
      and lists it in `withdrawn.json`.
    - Private turns always offer at least the calculator, so offered tool definitions now
      travel through the exporter and the worker. The worker renders them in the order the
      pinned llama-server does: transformers matched the server's `/apply-template` only in
      that order.
    - Recorded captures now keep tools too, and refuse replies generated without thinking
      explicitly off.

    Linux evidence:
    - The production builder and store passed 4 tests, and 10 removals each made one fail.
    - The capture tool passed 8 tests and the worker 13. Removing any of 6 Python protections
      made a test fail.
    - The real exporter carried a live capture and its tools to the worker, which refused it
      after its chat was withdrawn.
    - A tiny-model training run included a tool example.

    Windows CI (run 41178797203, commit `64da0c5`):
    - Every changed desktop, session and llama.cpp source built.
    - 148 of 149 tests ran; the other is the approved optional video skip.
    - These passed: `ReviaLiveCaptureTests`, `Revia.DatasetCapturePolicy` (all 8, including
      both live tests and the end-to-end export), `Revia.DesktopStopPolicy` with the
      unticked-box check, and the other branch checks.
    - `ReviaQualityFeedbackRuntimeTests` failed on its last assertion, through a test bug.
      The test switched the audience to public and back, then reused the judgment made
      before the switch. The session correctly refused it as stale, at admission, so the
      held-reply check the assertion named was never reached.
    - Commit `e8d8f1c` fixes the test. It now holds a fresh reply under the current audience
      before forgetting, and checks the reason for each refusal. In run 41181811601 the suite
      passed with its new checks. The only failures were the three that fail on `main`, and
      the intermittent tests passed.

    None of this has run against the companion on the host.

11. **Training corrections screen (2026-10-31).** Skills & Learning has a new **Training
    corrections** section. It lists kept captures only while the audience is private. Each
    capture shows what was said, the judged reply and the recorded judgment.
    - The owner saves a decision and the correct reply. Each save is a new revision under
      `RuntimeData/Learning/Captures/corrections/`, bound to the judged reply's digest.
    - `revia_dataset_capture.py candidates` reads saved corrections when `--corrections` is
      omitted, so no JSON is edited. Exporting stays a separate step.
    - **Delete capture…** deletes a capture and its correction, and lists it as withdrawn.
    - The session keeps the list in memory and rereads it only when the store changes, so
      the panel's one-second refresh does no file reading. A refresh never replaces a draft.
    - Building it exposed a withdrawal flaw. Withdrawing by the shared request would also
      have withdrawn a separately judged reply regenerated from the same request. Captures
      are now withdrawn by their own `displayed-reply:` id.

    Linux evidence:
    - The store passed 5 tests, and 6 more removals each made one fail.
    - The capture tool passed 9 tests, and 2 more removals each made one fail.
    - The real panel compiled against Qt 6.4.2 and passed a headless harness.
    - The window sources and the desktop test compiled.

    The session and desktop tests run in Windows CI.

    Windows CI run 41199765499 (commit `3181af9`):
    - The build stopped at linking `ReviaLiveCaptureTests.exe`. The link printed nothing:
      no linker message reached the captured build output or the job log.
    - The other 21 executables linked from the same freshly rebuilt foundation library,
      and no test ran.
    - Reproduced locally without that failure: MinGW-w64 GCC 13.2 and LLD 18.1.3 compiled
      the test with CI's flags and linked it. Its Windows build then passed all 5 tests
      under Wine 9, including the `ReplaceFileW` path a second correction revision takes.
    - Retry run 41203033290 (commit `ea226c6`, no code change) built cleanly, and 148 of 149
      tests ran. The session test and the desktop suite passed with the correction checks, as
      did every other branch check. The failures were the three that fail on `main` and the
      known intermittent `ReviaProactiveStateTests` temperature check.

12. **In-app dataset export (2026-10-31).** **Export dataset…** in Training corrections writes
    every decided capture as the next numbered dataset of the companion's lineage, such as
    `revia-live-3`, into a parent folder the owner chooses. The owner chose numbered exports
    with fixed 10% validation and 10% test.
    - The lineage in `RuntimeData/Learning/Datasets` keeps one salt and a copy of every
      export's manifest. An example keeps any partition an earlier export gave it, even
      after deletion, return and regrouping.
    - OneDrive, network and linked folders are refused. The withdrawal list is applied, and
      its path is named for the training job.
    - The C++ candidates match `revia_dataset_capture.py candidates`, pinned by
      `Tests/Fixture/liveCandidates.json`.
    - Building it exposed two grouping faults. Every capture listed `participant:owner` as a
      source, which put all of a person's captures in one partition, so no held-out set could
      ever form; the speaker is now recorded beside the sources. And the exporter grouped a
      whole app run together. On the owner's choice, a candidate naming its chat is now
      grouped by that chat.

    Evidence:
    - On Linux, the export passed 4 tests, and 8 removals each made one fail; 3 more
      removals covered the grouping fixes.
    - The same export tests passed as a Windows build under Wine 9. That run found that
      libstdc++ on Windows does not count UNC paths as absolute, now handled.
    - The tests caught a dangling loop over a destroyed temporary in the manifest merge.
    - The panel passed the headless Qt harness, and the window sources compiled.

    Windows CI run 41205772282 (commit `374fc20`):
    - The build passed, and 149 of 150 tests ran; the other is the approved optional video
      skip.
    - Every branch check passed, including `ReviaLiveDatasetExportTests`.
    - The session tests also passed. They export through the real Windows folder checks
      against the runner's temp folder.
    - The only failures were the three that also fail on `main`. The intermittent tests
      passed.

13. **Training jobs prepared from an export (2026-10-31).** `revia_training_worker.py prepare`
    writes the job for a dataset export and an installed base. Before, `job.json` was
    hand-written with its digests.
    - It reads every digest from the files and applies the run's own checks: job layout,
      base install manifest, dataset manifest and withdrawal recheck.
    - It uses the 4B configuration: `--smoke` gives 5 steps, and otherwise three epochs.
    - It names the companion's live withdrawal list, and never replaces a job.
    - Scratch and candidate folders inside OneDrive are now refused, when preparing and when
      running.

    Linux evidence:
    - The worker suite passed 16 fixture tests, and 4 removals each made one fail.
    - A smoke-prepared job with the full default configuration trained the tiny Qwen3.5
      with the real backend.

    Windows CI: run 41208914554 (commit `c2959cf`) built cleanly and ran 149 of 150 tests,
    the other being the approved optional video skip. `Revia.TrainingWorkerPolicy` passed
    under the runner's Python 3.12.10, including all three new `prepare` tests on Windows
    paths, as did every other branch check. Besides the three failures `main` shares, two
    tests failed, both intermittent (below): `ReviaProactiveStateTests` and, for the first
    time, `Revia.Foundation`. Since the previous run, only Python and docs had changed.
14. **Evaluation runner against the real quality tools (2026-10-31).** The runner had only
    driven Python stand-ins for `ReviaAnswerGateLive` and `ReviaAnswerQualityLive`. Windows
    builds of both tools now ran under Wine 9 against llama-server `b10453`, serving the tiny
    Qwen3.5 and its adapter. The run covered `plan`, both arms, `review` and `score`, on 14
    golden cases (two per category), the six-case holdout and the tool cases.
    - Found: in its default mode the quality tool answered as the fixed model name
      `Qwen3.5-4B-Q4_K_M.gguf`. The runtime's identity check refused the owned server, so all
      six holdout cases came back unjudged and no evaluation could ever have accepted. The
      tool now answers as the one model the server reports, as the cognition campaign already
      did, and names it in its report. The old name is only a fallback for a server that
      reports none.
    - The runner now counts a golden-set or holdout report only if it names the arm's own
      alias. Otherwise those cases are unavailable, and the decision says why.
    - With the fix, on the committed build, each report named its own arm. All 14 golden
      cases in each arm were judged, as were 11 of the 12 holdout results. The twelfth was
      counted as unavailable, not judged, because llama-server rejected the random model's
      output. The decision was reject, which is right for a random tiny model: no held-out
      improvement, and prefill 34% slower than base, outside the 5% limit. Retention found
      no regressions and was incomplete only for that one case.
    - The golden set and holdout run through the production runtime with the profile's own
      sampling (temperature 0.75; the gate seeds it with the plan's seed). Only the held-out
      replays and tool cases use temperature 0. The documentation now says so.

    Linux evidence: the runner suite passed its 8 fixture tests, and removing either the
    identity check or the reason it reports made the new test fail. The tools were
    cross-built with MinGW-w64 GCC 13.2 against a stub XML library, because this container's
    network policy refused the expat download. Neither tool parses XML, and Windows CI builds
    them with the real library.

    Windows CI: run 41221718153 (commit `0003651`, which carries this fix) built both tools
    with MinGW 13.1 and the real expat. It ran 149 of 150 tests, the other being the approved
    optional video skip. `Revia.AdapterEvaluationPolicy` passed with the new identity test, as
    did every other branch check. Besides the three failures `main` shares, two intermittent
    tests failed: `ReviaProactiveStateTests` (item 16) and `ReviaDelegationTests`.
15. **Adapter promotion deferred (owner's decision, 2026-10-31).** Adapters stay
    evaluation-only, and nothing was built to serve one. No C++ or PowerShell source passes
    `--lora`; only the evaluation runner's own llama-server does. How an accepted adapter
    would be used is designed only after a candidate's `decision.json` accepts. For a 4B
    adapter that is also the separate decision to run a 4B model in Revia at all. Rejected
    alternatives: a gated 4B Main profile with receipts and rollback, and a separate 4B tier
    beside the 9B Main.
16. **Intermittent proactive-state failures (2026-10-31).** `ReviaProactiveStateTests`, and
    `Revia.Foundation`, which compiles the same `Tests/proactiveStateTests.cpp`, failed at
    random on three different reply-temperature checks. Product sampling was never wrong.
    - Cause: after a private turn, the memory agent classifies it on its own background
      thread. That classification is a JSON-mode request at temperature 0.1. The test backend
      recorded it like any other request, so when it landed after the reply, `Last()` returned
      it in place of the reply. A diagnostic Windows build under Wine showed it arriving between
      turns.
    - The same race could take the backend's one-shot `rejectNext`, end its background-vision
      hold early, or shift `Count()`. Worse, a privacy check reading `Last()` could examine the
      classification instead of the reply and pass wrongly.
    - Fix: the backend recognises the classification by its envelope: JSON mode, no stop
      markers, and one user message carrying `user_message`. It answers it as before and keeps
      it apart from the requests every check reads. All 64 `Last()` checks keep their wording.
    - A new test waits until the real agent's classification has arrived after a private
      turn, which makes the losing order certain. It then checks that the reply is still
      `Last()`. Recording classifications as before failed it in 3 of 3 runs. It also fails if
      the classifier's envelope changes and the backend stops recognising it.

    Evidence, as a Windows build under Wine 9: the unfixed suite passed 60 of 60 runs, three at
    a time, so the race needs CI's timing. The fixed suite also passed 60 of 60.

**CI.** `main` had been failing the Windows CTest inventory gate before any test ran.
`Install Qt` re-runs `setup-python` and moves the job to CPython 3.15 without numpy, so
`Revia.RetrievalBenchmarkContracts` was never registered. Configure now pins
`Python3_EXECUTABLE` to the job's own interpreter. In run 41159287582 (commit `1e00faa`),
145 of 146 tests ran; the other is the approved optional video skip. All five of this
branch's checks passed: `ReviaPromptBatchSettingsTests`, `ReviaDatasetExportTests`,
`Revia.TrainingWorkerPolicy`, `Revia.TrainingAssetInstaller` and
`Revia.AdapterExportPolicy`. The other failures had been hidden on `main` behind the gate.

Failing in both runs:

- `ReviaAudiencePrivacyTests`: the known obsolete `Reference-image` expectation, which the
  host candidate patch addresses.
- `Revia.NliDedupBenchmarkContract`: compares the runner's 8.3 short temp path with its long
  name.
- `Revia.VideoPreview`: reports SKIP without FFmpeg but exits as a failure.

Intermittent:

- `Revia.ObservedLocalModel`: its credential-refusal check failed in run 41156920820 and
  passed in run 41159287582, on identical C++ sources (commits `58335fd` and `1e00faa`).
- `ReviaProactiveStateTests`: passed once, then failed in runs 41159287582 and 41164612553.
  A request after a prior native source finding lacked the profile's 0.75 temperature. It
  passed in runs 41175961328 and 41178797203, and failed again in run 41208914554. In run
  41221718153 it failed at a third check of the same kind, "An unavailable saved reference
  changed ordinary conversation sampling". Cause found and fixed: item 16.
- `Revia.Foundation`: failed once, in run 41208914554, at "A verified private saved reference
  changed a public reply's sampling". That check is in `Tests/proactiveStateTests.cpp`, which
  `ReviaTests` also compiles. It had the same cause, fixed in item 16. The C++ sources were
  identical to run 41205772282, where it passed.
- `ReviaDelegationTests`: failed in run 41175961328 (commit `4df13d5`) and again in run
  41221718153 (commit `0003651`), at `TestWorkersYieldToTheConversation`. An analysis worker
  preempted by the conversation failed instead of retrying. Neither commit changed delegation
  code, and the test passed in the runs between.

Of these, `Revia.ObservedLocalModel`, `ReviaDelegationTests` and the three tests failing in
both runs remain open as a separate queued task. The proactive-state failures, which also
reached `Revia.Foundation`, are fixed on this branch (item 16).

## Wrap checkpoint, 2026-10-30, D5

The owner requested wrapping up with 6% usage remaining. Further model trials
stopped. **C → D → E is not fully complete.** The normal Desktop remains the
accepted C8 build; D5 has not been deployed and normal video stays disabled.
No staging, commits or pushes were performed.

The latest D5 build passed all 36 requested targets, with all 1,201 recorded
inputs unchanged during compilation. Six final focused checks passed in 4.36s:
media selection, reminder intent, video publication retirement, generated-media
runtime, and both Python service policies. The D4 four-check regression group
also passed, including ToolLoop/ProactiveState and the real fresh-PNG animation
source regression. The host CPU video provider/import checks passed. These are
scoped results; the complete final Foundation/CTest suite has not passed.

Actual accepted image evidence is in the D4 native runs and Desktop generation
run: text-to-image, pixel inspection, source-bound image-to-image, comparison,
Regenerate/Edit, archive restoration, Forget and native unavailable responses.
The restored original now sends its exact PNG; forgotten versions send no pixels
and receive host-owned absence replies. The actual Qt run passed five Send turns,
verified Canvas/card publication and source provenance, then removed access on
Forget. Its run took 55.187s. The generated blue-coat anime PNG is real; reference
variation preserves general composition but changes details and does not precisely
deliver the requested evening palette. Evidence includes
`native-v4-image-continuity-provisional-audit.json`,
`native-v4-image-native-absence-supplement.json` and
`desktop-generation-runner-receipt.json` under `build/stage-d-20261030`.

The D4 native I2V worker actually generated 49 frames, and native verification
passed, but publication failed because OneDrive marked long-lived private scratch
directories with reparse tag `0x9000e01a`. The unpublished output was withheld.
D5 moves private scratch to validated local TEMP, retains strict reparse guards,
and uses exclusive bounded publication across volumes. Native RED/GREEN and eight
Python checks passed; **the genuine long-running D5 rerun is still pending**.
The three fresh D5 video workflows are prepared but unrun. Native T2V/cancellation
and actual generated-video Qt completion/playback/follow-up remain pending.
The source-equality receipt `d5-image-evidence-source-reuse.json` proves that only
the isolated video provider/worker changed between accepted D4 image code and D5.
The failed D4 I2V result remains failed and retained.

Actual companion checks verified a real saved-file watcher comment in 5.364s,
its native source follow-up in 80.193ms, duplicate suppression, stale/incomplete
rejection, pause, and an eight-second idle Focus window with explicit Focus
suppression before direct input. Windows SAPI playback interruption and queued
bank replay passed, stopping in 62ms. Acoustic microphone detection is unverified.
Ordinary chat during actual I2V generation answered correctly in 1.281s.

**The ordinary dialogue campaign failed quality acceptance.** The image-feeling
answer did not answer the personal question, code generation repeated malformed
function headers, and a resume reply repeated an older topic. Raw provider output
contains the code failure; it was not transport/token truncation. The Morgan
exact-body study failed both with and without duplicate Working continuity, so
the proposed Core change remains unshipped. A DRY-only exact request pair is
prepared, not run; its causal hypothesis is unproved. Authored profile and saved
temperature remain unchanged. See `build/stage-e-20261030` for both independent
watcher audits, the SAPI receipt, native dialogue and diagnostic preparation.

Next smallest steps: finish the remaining obsolete Audience test expectations
using the prepared strict candidate and host verification; run the final affected
suite/Foundation; qualify the prepared D5 three-video cohort and actual Qt video
path; investigate the retained dialogue failure with controlled requests. Then
make a scoped deployment decision. The Audience candidate is still ignored and
unapplied; tracked tests retain obsolete `Reference-image` assertions. Its corrected
object is compiled, but needs relinking against the completed D5 archive and a
host run. Exact command arguments are in
`build/stage-d-20261030/audience-stage-d-capabilities-verification/build-receipt.json`.
No unqualified video route was enabled.

Requested scope: continue the existing companion implementation in small runnable
stages; the attached master specification is the acceptance authority. Preserve
personal data and leave work unstaged/uncommitted. The owner separately approved
the one 9B model download and local qualification recorded below.

## Remaining stages continuation, 2026-10-30 (in progress)

The owner requested all remaining stages and explicitly confirmed **C → D → E**.
Stage B remains the completed bounded checkpoint below. Continue in the actual
checkout; preserve personal data and leave all changes unstaged/uncommitted.
The implementation plan is
`docs/superpowers/plans/2026-10-30-stages-c-through-e.md`.

Latest checkpoint (C8): **Stage C's bounded local video path is verified and
deployed**. All 18 affected targets plus the ordinary streaming check and CLI
target built. All 29 frozen source inputs stayed byte-identical during the main
build. All 17 selected CTest checks passed (120.66 seconds), Foundation passed
(194.52 seconds), and the actual store/decoder check passed 72/72. Fresh affected
Speech and Qt runs passed independent source/content review: 4/4 native and
12/12 Qt PNGs matched their actual source RGB pixels and presentation times.
Every Qt reply retains its four observation rows and both sampling-limit
sentences. The terminal negative-audio request now uses the bounded visual path.

Only `build/debug/ReviaDesktop.exe` was replaced; 22 runtime files were retained.
The normal-path exact executable fingerprint and UI-only smoke passed. The
profile, `Config/settings.json` and owner overlay match their original hashes.
The deployment backup supports rollback. This checkpoint does **not** turn the
retained ASR golden-word, strict font-size OCR or ordinary Stage E failures into
passes. Evidence: `build/stage-c-20261030/stage-c-bounded-acceptance.json`, the
four `native-v5-build8` / `desktop-video-v5` independent audit receipts,
`desktop-deployment-verification.json` and
`desktop-deploy-backup-543f342137cc427ead17d555fd4ac5ed/receipt.json`.

Stage D implementation now includes typed reference-guided image requests,
strict generated-artifact import and assistant associations, chat publication,
authorized follow-up sources, and an owned asynchronous video route. The integrated
parent build and full native/Qt acceptance are still pending. CPU provider and
library checks passed independently; they do not establish the full application path.

The approved pinned LTX candidate has produced one text-to-video clip and three
corrected image-to-video controls: each contains 49 readable frames at 384×256,
25 fps, or 1.96 seconds. Generation took about 153–165 seconds. Actual Main chat
turns during these separate qualification jobs took about 1.4–1.6 seconds and
retained the same Main process. The first I2V attempt failed on a generator-device
mismatch; the corrected route uses the secondary CUDA device. Outputs demonstrate
working generation, but requested objects can be unclear and source details drift
between frames. Normal video configuration remains disabled pending integration
review. Original qualification monitoring measured the Python launcher rather
than Torch worker RSS; the corrected review labels that limitation explicitly.
Evidence: `build/stage-d-20261030/ltx-native-trials-independent-review-v2.json`.

The installed SD-Turbo provider also produced an actual 512×512 anime-style PNG
in 29.0 seconds including loading, using four diffusion steps. The visible image
has illustrated/cel-style features, but missed requested hair and prop details.
This establishes basic local anime-style bitmap generation, without certifying
polished anime or character fidelity. Receipt: `anime-native-run1.console.json`
under `build/stage-d-20261030`.

Stage E remains unfinished. Narrow repairs now cover nonblocking chat verification
during media execution, current-question emotion classification, explicit reminder
intent, and original-epoch/quiet-mode handling of generated completions. Fresh
integrated execution and realistic companion acceptance are required before these
remaining stages can be called complete. No work has been staged or committed.

Earlier C7 checkpoint: the actual native run completed 13
workflows and 32 turns. All 16 required visual-fact checks and 11 source,
lifecycle and coverage controls passed. The independent audit matched all 68
transmitted PNGs to the decoded source pixels and timestamps. Pure transcript
requests now return the actual timestamped ASR hypothesis without a conversational
model rewriting it. Four such deliveries were faithful, while three golden-word
checks still fail because the recognizer hears "Courts" for "Quartz"; uncertainty
is retained and the words are not patched to match the fixture. A hostile clip
retained the instruction boundary but included small text beyond the requested
large label. One isolated schema projection trial is pending. Ordinary companion
replies still have Stage E failures; the full original oracle set is not green.

C7 built all 18 affected targets. Sixteen selected CTest checks, the Foundation
aggregate (195.59 seconds with its normal 600-second budget), and all 72 actual
store/decoder checks passed. The build's strict byte freeze correctly reported
two signature-format changes during compilation; independent reconstruction and
token comparison proved those two files have identical C++ tokens. That raw
failed byte gate remains in the evidence. C8 will use a fresh strict freeze.

The actual Qt C7 run completed Attach MP4, interval follow-up, trusted playback
and WebM drop/send with correct visual rows. It exposed repeated sampling notices
being suppressed on later turns. A focused runtime repair and negative-audio
routing repair are implemented and awaiting C8 linked/native/Qt verification.
The optional-video CI policy repair passed all 25 build-driver fixtures in the
host environment; missing optional decoder prerequisites remain explicitly
unverified rather than being counted as executed tests. CTest discovers 135
checks; this is an inventory, not a claim that all 135 ran.

Evidence is under `build/stage-c-20261030`: `stage-c-build7-receipt.json`,
`build7-semantic-source-acceptance.json`, `ctest-build7-components-junit.xml`,
`ctest-build7-foundation-junit.xml`, `store-live-run3-c7/receipt.json`,
`native-v4-build7-independent-mechanics-corrected.json`,
`native-v4-build7-independent-content.json` and
`optional-video-ci-final-host.console.log`. Earlier failed trials below remain
historical evidence. The normal Desktop still uses the accepted Stage B binary;
Stage C has not been deployed.

The owner approved the exact 25.39 GB pinned native LTX-Video 0.9.5 assets and
4.71 MB of isolated runtime packages at
`D:/ReviaModels/LTX-Video-0.9.5-e58e28c`. All 22 allowlisted files were downloaded
and verified. The three local wheels were installed only into that root's
`_runtime/site-packages`; offline imports and offload/tiling API checks passed,
and existing package metadata remained unchanged. No LTX weights have been
loaded and no video has been generated. The prepared qualifier is not inference
evidence. Normal-route selection remains conditional on actual generation,
source conditioning, cancellation, integration and resource measurements.
Evidence: `build/stage-d-20261030/ltx-0.9.5-native-proposal`, the destination's
`_revia_download/20261030T193030Z-f7a01545-receipt.json` and
`_runtime/preflight.json`.

### Earlier Stage C checkpoints (superseded by the latest checkpoint above)

Stage C now extends the existing attachment owner with explicit private video
intake, bounded MP4/H.264 and WebM/VP8/VP9 decoding, source-linked timestamps and
interval reinspection, video archive metadata and an in-app silent preview.
These integrations are **implemented but not yet end-to-end verified**. The
standalone actual decoder checks passed for MP4/H.264 and WebM/VP9 pixels,
timestamps, corruption/limits/cancellation, paced previews and bounded AAC-to-PCM
extraction. Optional file transcription and actual Qt/model acceptance are still
in progress. Do not call Stage C complete from these component checks.

The existing image attachment and selection checks passed before changes.
Initial branch, HEAD, working-tree and normal profile/settings hashes are recorded
in `build/stage-c-20261030/initial-state.json`. Scoped selection checks now cover
clock-time versus video routing, transcript follow-ups, clip ordinals, interval
validation and image/video separation. Installed FFmpeg/ffprobe were copied with
hash-verified receipts and accompanying licenses into the ignored local
`ThirdParty/ffmpeg/bin`; no decoder download was needed. Constructed neutral local
clips and their independent content/timing oracles are retained under
`build/stage-c-20261030/fixtures`. The normal Desktop deployment remains the
accepted Stage B executable until the new stage is verified.

The first actual native video trial is retained as a failure, not acceptance:
four completed workflows produced 14 replies, with nine definite factual or
grounding failures, four required-fact passes and one correct invalid-time
rejection. An independent CPU pixel audit matched all 56 transmitted PNG parts
to the actual clip frames and timestamps. Interpretation failures included
full-frame center versus object-group center, unsupported motion/duration
claims, speech versus OCR confusion, and promoting uncertain tone-only ASR
hypotheses to clear speech. A separate archive fixture had restoration disabled;
its incomplete lifecycle run supplies no restore/forget evidence. The next
candidate adds factual-video guidance, broader guarded what-happens routing and
a metadata-only sparse-motion limitation. Fresh native/Qt acceptance is pending.
Evidence: `build/stage-c-20261030/native-v1-independent-audit-20261030-v2.json`.

The actual store/decoder acceptance check passed all 56 content and lifecycle
checks in `store-live-run1/receipt.json`. This includes the brief event missed by
wide samples and found by a fresh interval sample, source identity, cancellation,
tamper/missing-source/forget behavior and unchanged inputs. It does not qualify
the language model's interpretation. The Windows CTest inventory now contains
134 checks, including six new video checks and structured-data reply preservation; discovery count is not an
execution or pass count.

Further review and actual C4 probes reproduced and repaired interval sampling
outside the selected bounds, sent/orphaned Qt poster caches after Forget, and
counting decoded video frames as uploaded images. Verbose provider content proved
that native prose cleanup removed repeated per-frame facts and inserted spaces
inside decimal timestamps; a generic structured-row/decimal repair preserves all
six captured provider reports in isolated checks. A four-call controlled 9B
study showed that older dialogue/template interference contributes to center
errors. The next provider request omits older dialogue only for self-contained
factual video inspection, retaining the unchanged system/current evidence and
all stored canonical dialogue. Prior-state, creative, personal and still-image
controls pass isolated wire tests. A separate generic-format experiment failed
and its instruction is excluded. C5's complete native/Qt acceptance is pending.

Uploaded transcription now explicitly requests the small local Silero 5.1.2 VAD
asset. Fifteen actual CPU comparisons show suppression of the tone-only false
positive, while proper-name/word errors remain. The 885,098-byte hash-verified
asset and its pinned MIT model card were added under ignored `Models`, with a
local installation receipt; normal microphone settings were preserved.

C4's selected linked checks passed 14 of 15; the Foundation aggregate exceeded the
180-second budget used for that trial and requires the normal 600-second rerun.
Its timeout is preserved separately. C5 rebuild/verification is in progress.

Stage D recon found that the installed SD-Turbo/SDXL-Turbo weights and Diffusers
support image-to-image reuse; no new model is needed for that candidate route.
Reference editing is not implemented or qualified yet. No installed temporal
video-generation weights/provider were found in the inspected configured caches;
those routes remain unavailable pending exact prerequisite/resource review.
Stage E's ordinary/current-request and creative failures remain open; read-only
recon is retained in `build/stage-c-20261030/stage-e-recon.md`.

The earlier LTX proposal was subsequently approved and installed as recorded in
the latest checkpoint above; the earlier unavailable-runtime assessment describes
the state before that approved installation.

## Verified Stage B checkpoint, 2026-10-30

Stage B's bounded private image-upload and factual follow-up path is verified and
configured for normal use. Candidate11 supplied the required visual facts and
source/lifecycle behavior in all 19 original native workflows (42 required media
turns), followed by three actual Qt turns through Attach, clipboard paste,
drag-and-drop and Send. Actual accepted pixels, digests, IDs and wire order were
checked; the Qt requests carried 1, 1 and 2 images. The full primary audits retain
ordinary failures and extra-answer limitations separately from scoped acceptance.

The existing attachment owner selectively reopens validated private copies.
Messages and archives retain ordered IDs, digests and metadata, without pixels or
original paths. Follow-up selection, Edit/Regenerate/Retry, attributed continuation,
forgetting and pending-turn admission use those scoped references. New-chat,
public/shared, forgotten, missing, tampered and unavailable-model controls withhold
unavailable pixels. Reinspection still works after the original turn leaves the
text window. Desktop intake captures its originating session and joins before
retirement; its chip/staged state and transmitted content were verified.

The approved matching LMStudio Qwen3.5-9B Q4_K_M/BF16 pair is installed and selected
for Main. The reviewed helper changed only the three Main model strings in
`Config/settings.json`. The supported owner control enabled `llm.visionEnabled`
through the sole value in `RuntimeData/Settings/owner-settings.json`; the base
vision default remains false. Screen and continuous awareness remain false.
The authored profile is byte-identical and its saved temperature remains 0.75.
Effective Main settings retain 32K context and the 1,024-token image minimum.
Private test memory/speech were disabled in isolated
roots; no test conversation was added to personal stores.

Factual pixel requests use request-local temperature zero/no DRY and omit the
extra conversational posture, retaining authored SYSTEM, role history and actual
reference evidence. Creative, personal and prior-dialogue-dependent requests keep
full state and saved sampling. Repeating the safely prepared current question
after image guidance repaired a reproduced stale-caption label nonanswer without
removing history or changing identity. Named inspection, image separators, bold
speech preservation and native availability/ambiguity/count replies also have
regression coverage. Earlier failed cohorts and controlled experiments remain
failed evidence rather than being counted as passes.

The final9 five-target build completed. All 14 selected checks passed, including
fresh AudiencePrivacy, Desktop stop/smoke and Foundation (190.543 seconds), with
ten unaffected checks retained by exact receipt identity. The final inventory
matches all 127 registered names; all 127 were not executed. Foundation is not
claimed to execute the standalone ToolLoop or Audience suites. Three real local
generated PNGs, cancellation and reference-edit refusal retain separate passing
artifact/content evidence.

The accepted Desktop executable and one runtime DLL are deployed to `build/debug`;
21 other program files matched already. Backups, hashes and normal-path isolated
UI smoke exit zero are retained. Launch with `Tools/RunDesktop.ps1`, which selects
the repository's explicit runtime root and effective owner overlay. The UI smoke
uses no personal companion session; live image checks used isolated actual Qt
sessions. Final cleanup found zero relevant processes/listeners, all 23 deployed
files matching the plan, the six frozen source hashes intact and the profile
unchanged. Nothing was staged, committed or pushed; HEAD remains the original
`e0d9ccb4e7db9ddb08f5884f8bd254b1b49eed56` on main.

The fresh bounded watcher regression passed: save-to-comment 5.265 seconds,
admission-to-comment 4.180 seconds, direct follow-up 74.94 ms, zero duplicates and
paused-save refusal. Its unchanged native source path reused the declared final8
Awareness binary and used zero substantive model requests. General model code
review accuracy remains unqualified.

On the RTX 5070, 32 native pixel replies had median latency 2.074 seconds and a
4.713-second maximum. Observed total device use peaked at 8,557 MiB; owned-server
RSS sampled 6.991 GB with a 7.071 GB lifetime peak. The actual Qt workflow took
23.188 seconds including 6.516 seconds server startup. These are bounded workload
measurements, not cold-start or simultaneous generation/voice guarantees.

Known limits remain explicit: the GIF answer fulfilled its current request but
also answered an older JPEG question; approximate geometry, speculative label
meanings and chatty/self-certifying prose persist. The ordinary restored Morgan
introduction still drifts to an old task, and the separate personal image-feeling
control repeated a poem. Broader creative/ordinary conversation and personality
quality remain open for Stage E. Pre-send screenshots preceded queued chip layout;
chip counts and payloads prove intake, not complete visible-chip rendering.
Universal OCR, physical audio and combined media load are unqualified. Video,
reference editing and video generation remain later-stage work.

Local evidence, including failures and rollback backups, is retained under
`build/stage-b-20261029`. The final completion report is
`stage-b-completion-evidence.final9-candidate11.json` in that directory.

## Verified Stage A checkpoint, 2026-10-29

The Stage A baseline and declared bounded native saved-file workflow are verified.
The twelve-case native corpus and three fresh final watcher confirmations preserve
correct findings, follow-ups, explicit silence, duplicate/pause gates and owned
shutdown. Final comments took 5.217–5.274 s from real save, with 68–77 ms native
follow-ups. Six final ordinary controls answered without false capability denial;
one opinion preferred a third alternative, which remains recorded in the audit.

All eleven selected final CTest suites passed, including the rebuilt full
Foundation suite (176.73 s). The 124-registration inventory matches exactly;
the entire 124-test inventory was not executed. Independent evidence:
`build/stage-a-review-20261029/stage-a-completion-evidence.json` and
`native-final-live-audit.json`. Cleanup records zero owned qualification processes
and no listeners on their three test ports. Nothing was staged, committed or
pushed; the normal 4B/profile configuration is unchanged and the approved 9B is
retained but unselected.

This qualifies the declared native C++ checks and observed workflow. General
model code-review accuracy, broad personality/creativity quality, physical voice,
live Focus comfort and combined media load remain separate open qualifications.
The next unfinished implementation stage is **Stage B: image upload/follow-up
understanding**, starting with isolated qualification of the installed 4B/projector
pair and scoped message associations for existing stored attachment IDs. Stage B
implementation had not begun at that historical Stage A checkpoint. The current
Stage B implementation and qualification status are recorded above.

## Baseline, 2026-10-29

- Branch `main`, commit `e0d9ccb4e7db9ddb08f5884f8bd254b1b49eed56`; working tree
  clean before this continuation. Work stays in the requested actual checkout.
- RTX 5070: 12,227 MiB total, 1,484 MiB used, 10,461 MiB free at inspection.
  A separate RTX 2070 SUPER reports 8,192 MiB total and 7,989 MiB free. Driver
  616.64. Windows reports 98,211 MiB usable RAM and approximately 75,266 MiB free.
  These are instantaneous system measurements, not model allocations.
- Installed GGUFs include Qwen3.5 4B and 0.8B, Qwen3-VL 8B and its named
  projector, and Qwen2.5-Omni 7B. Existence does not establish compatibility.
  Owned llama.cpp, ImageGen and QwenTTS runtimes exist. No Revia/model/Python
  process was observed in the initial process inventory.
- The checked-in profile is Revia's existing authored identity. Settings select
  Qwen3.5-4B, 32K context, 1,024 output and 3,072 thinking tokens; primary vision
  and expert routing are disabled. Image generation is enabled. Saved-file API
  is disabled/untrusted. Owner overlays, companion preferences and profile
  overrides apply at startup; these defaults are not live effective settings.
- Earlier `.revia-ai` progress is stale (September); the newer
  [editor awareness evidence](EDITOR_AWARENESS.md) explicitly leaves Stage A
  partial after an incorrect live finding. Historical October 6 art and context
  runs in [the integration ledger](superpowers/plans/2026-10-06-capability-integration.md)
  retain their original limits.

## Requirement status

Source inspection establishes connections, not live acceptance. Status will be
updated with fresh build/test and model evidence below.

| Requirement | Status | Existing owner / evidence | Smallest remaining action |
|---|---|---|---|
| Identity, prompt assembly, memory, scoped continuity | Verified native; live continuity sample passed | Existing profile unchanged; Foundation regressions and case-04 contextual follow-up | Personal memory was disabled in isolated live runs; qualify ordinary sustained conversation separately |
| Stable saved-file observation and unsolicited reply | Verified declared native subset and workflow | Twelve-case corpus, final real-watcher confirmations and bounded Stage B regression | Keep the bounded regression; general model code judgment remains unqualified |
| Normal/Talkative/Focus, duplicate/stale/privacy gates | Verified native; duplicate/pause exercised live | Mode, saved-source, editor-session, Foundation and Desktop stop suites passed | Live Focus comfort, false interruptions and broader negative scenarios remain unqualified |
| Voice interruption and direct-input priority | Verified native; physical audio unverified | Existing Foundation/SpeechCoordinator and editor-session cancellation checks passed | Separately qualify physical microphone/speaker interruption |
| Structured IDE/live unsaved buffers, diagnostics/build events | Partial | Current editor integration observes one selected saved C/C++ file only | Add integrations only after saved-file route qualification |
| Initial image intake and actual vision payload | Verified bounded Stage B scope and configured | Approved matching 9B pair; final 19-workflow native cohort and actual Qt Attach/Paste/Drop | Retain bounded limits; universal OCR and broader creative/personal quality are unqualified |
| Image follow-up/Edit/Regenerate/Retry/restore | Verified bounded source, lifecycle and factual-answer scope | Ordered message/archive references; selective validated pixels; replay, restoration, forgetting and admission checks | Keep scoped regressions; ordinary topic drift and personal image conversation remain Stage E quality work |
| Video intake, interval sampling/transcription/playback | Missing | Attachment kinds currently Image/Text/Pdf/Docx | Bounded decoder route through existing attachment owner |
| Local text-to-image | Verified local retest | Current native live driver generated, cancelled, released and restarted SD-Turbo on RTX 5070 | Simultaneous chat/voice and visual prompt fidelity require separate acceptance |
| Reference editing and image/text-to-video | Missing | Existing generator is text-to-image only | Refuse silent reference downgrade; qualify installed provider/workflow separately |
| Generated-media chat cards and reinspection | Partial | Verified Canvas publication exists; no complete scoped media conversation | Connect verified artifacts through existing attachment/understanding route |
| Resource planning, cancellation, integrated load quality | Partial | Existing ResourcePlanner and owned workers | Measure real companion/media coexistence after individual routes work |

## Original Stage A continuation plan

The first unfinished requirement is Stage A live qualification. Reuse its existing
event, attention, prompt, private-context and cancellation paths. Keep the authored
personality and current owner's settings intact. Use fresh isolated runtime data
with the existing local model and only an explicitly selected disposable saved
source; disable auxiliary models, audio, network tools and screenshots for the
initial text-only qualification. No attention or resource gate bypass.

- [x] Build the current source using the Qt-matched local preset and record the
  existing relevant regression baseline before product edits.
- [x] Run the existing `ReviaCompanionAwarenessLive` against a known defect and
  a safe saved source. Inspect the actual comment, silence and follow-up quality;
  fixture success or HTTP 202 is not semantic acceptance.
- [x] Reproduce and minimally repair defects revealed by these checks. Add a
  meaningful failing regression before runtime changes, then rerun affected tests.
- [x] Review the final diff independently, record commands/results/limitations,
  and identify the next unfinished step. Keep Stage A partial if a required real
  behavior is still unqualified; do not substitute a scripted demonstration.

Build evidence is retained under `build/companion-*-20261029.log`; this file is the
single continuation record for the current upgrade. Existing runtime data and
historical evidence are preserved.

## Changes and regression evidence

- `Private/Perception/savedFileObservation.cpp`: recognize numeric separators and
  raw string prefixes/delimiters in the existing completeness filter. Previously
  `1'000` and raw strings containing an unmatched ordinary quote could suppress a
  complete save, while an unfinished raw string could be admitted. This remains
  a bounded lexical nomination filter, not a parser or proof of correctness.
- `Tests/savedFileObservationTests.cpp`: decimal/hex/binary/fractional separators,
  genuine character literals, UTF-8 raw prefixes, delimiter bounds, incomplete
  raw strings and macro adjacency. The original native suite passed before
  edits; the added cases failed, then passed after the fix. Independent review
  caught raw detection inside `PREFIX_R`; its reproduction also failed before
  the correction and passed afterward. Accepted examples additionally compiled
  with `g++ -std=c++20 -fsyntax-only`. Final standalone validation passed with
  `-Wall -Wextra` and no diagnostic output.
- `Tools/Quality/companionAwarenessLive.cpp`: add `--expect-silence`, require a
  native `Silent` event and zero initiative comments through completed shutdown,
  preserve failure for suppression/timeout/unexpected or late comments, and
  record the expected/observed outcome. It still leaves semantic qualification
  separate from mechanical success. Unknown/repeated flags are refused.
- `docs/EDITOR_AWARENESS.md`: document the filter and explicit live expectations,
  and replace the stale latest-preflight account with this qualification.

The current Desktop, native session/live harness, Foundation and focused targets
built using Qt 6.8.3 with its matching MinGW 13.1 compiler. GNU linking the 1.2 GB
Debug foundation archive was slow. The final verification cache uses six compile
jobs, three link jobs and `-Wl,--strip-debug`; compiler Debug settings remain
unchanged. These cache settings do not change checked-in presets or runtime
configuration. Interrupted early builds are not recorded as successful builds.

Commands (from the repository root):

```powershell
cmake --preset debug-qt13-local -DREVIA_COMPILE_JOBS=6 -DREVIA_LINK_JOBS=3 '-DCMAKE_EXE_LINKER_FLAGS=-Wl,--strip-debug'
cmake --build build/review-5c9cfb2/clean --parallel 8 --target ReviaCompanionAwarenessLive ReviaInitiativeModeTests ReviaSavedFileObservationTests ReviaEditorAwarenessTests ReviaTests ReviaDesktop ReviaDesktopStopTests ReviaImageGeneratorTests ReviaImageActionTests ReviaImageArtifactTests
cmake --build build/review-5c9cfb2/clean --parallel 4 --target ReviaImageGenerationLive
ctest --test-dir build/review-5c9cfb2/clean -R 'Revia\.(EditorObservationScripts|ImageServicePolicy)$' --output-on-failure
ctest --test-dir build/review-5c9cfb2/clean -R '^(Revia(InitiativeMode|SavedFileObservation)Tests|Revia\.Image(Generator|Actions))$' --output-on-failure
ctest --test-dir build/review-5c9cfb2/clean -R '^ReviaEditorAwarenessTests$' --output-on-failure
ctest --test-dir build/review-5c9cfb2/clean -R '^Revia\.DesktopSmoke$' --output-on-failure
ctest --test-dir build/review-5c9cfb2/clean -R '^Revia\.(Foundation|DesktopStopPolicy|ImageArtifact)$' --output-on-failure --timeout 600
```

All **11 selected CTest suites passed**. Foundation took 178.52 s; Desktop stop
policy took 13.28 s. Initial baseline evidence consists of the original native
saved-source suite and the script/provider policy suites; no full pre-edit
Foundation result is claimed. An early editor-suite launch was `Not Run` because
its executable was still being linked; the completed current build passed it.
Deliberate new RED regressions are distinct from that build collision. No
remaining failure was observed in the completed selected checks. The final test
loop warning cleanup was also rebuilt/tested in the standalone validator.

Logs: `build/companion-final-build-20261029.log`,
`companion-script-baseline-20261029.log`, `companion-focused-20261029.log`,
`companion-editor-current-20261029.log`, `companion-desktop-smoke-20261029.log`,
`companion-regressions-20261029.log` and `companion-image-live-build-20261029.log`.
The full repository CTest inventory was not run; the full Foundation executable
and the selected component/Desktop suites were run.

## Real-model qualification

Evidence root: `build/companion-evidence-20261029/`. Neutral `Source/sample.cpp`
paths supplied the actual source, without expected-outcome labels. A separate
compiled fixture established that the `<=` loop throws `std::out_of_range` for
absent/empty inputs, while the corrected `<` loop handles present, absent and
empty cases. The authored profile text was copied unchanged, with only memory
and speech disabled for isolation. Cloud, screenshots, auxiliary models,
effectful tools, voice and archive were disabled; the owner's settings, memories
and model selection were not changed.

| Case | Actual model / budget | Observed outcome | Qualification |
|---|---|---|---|
| `case-01` | Qwen3.5-4B Q4_K_M; 32K context, 3,072 thinking + 1,024 answer | Real watcher save admitted in 1,151 ms; triggered after 5,170 ms; 4,096-token generation exhausted and was suppressed after 41,871 ms from save | Real watcher/authorization path passed; meaningful comment/follow-up failed |
| `case-02` | Same model; external watcher mode | Harness timed out before the operator issued the save | Test coordination error; **not** a safe-code model result; retained and excluded from judgment counts |
| `case-03` | Same model/budget; native saved-event submission | Explicit native silence after 30,212 ms, zero comments through shutdown | Appropriate silence for the corrected fixture; expected-silence mode passed |
| `case-04` | Same model/budget; backend verbosity 5 | Correct invalid-index/`at()` finding after 31,006 ms; direct follow-up named `ContainsValue` and corrected `<=` to `<` in 1,488 ms | Real unsolicited comment and contextual follow-up passed for this sample; initial remark did not name the function |
| `case-05` | Same model, isolated 512 thinking + 1,024 answer | Exhausted all 1,536 tokens; suppressed after 17,611 ms | Shorter budget did not qualify as a responsiveness repair; not applied |
| `case-07` | Installed Qwen3-VL-8B Q4_K_M, **text only**, same profile/context | Chose explicit silence on the defective fixture after 4,913 ms; about 8.6 GiB system-wide GPU use at the final snapshot | Fast false negative; not qualified as a replacement and not selected |

`case-06` and `case-08` were prepared but not run after their corresponding
candidates failed; they are excluded from results. Trials are not a general
accuracy estimate. Successful cases do not erase the failed defect trials.

Case-04's actual follow-up: “The smallest correction is changing the loop
condition from `i <= values.size()` to `i < values.size()`. Since `values.at(i)`
will throw if `i` equals the size, the upper bound must be exclusive.” This agrees
with the independently compiled fixture. No canned reply or fabricated model
result was used.

The installed llama.cpp executable reports build **10453**, commit **3cb7ffb1a**.
Its exact [request implementation](https://github.com/ggml-org/llama.cpp/blob/3cb7ffb1a1f612d5e4a46244ae5a3c77ad934a70/tools/server/server-common.cpp#L1335-L1349)
supports `reasoning_budget_tokens`. Case-04's native diagnostics show activation
at 3,072 tokens and forced closing of the reasoning sequence. The client field
was therefore retained. Case-01 generated 4,096 tokens in 35,648.58 ms at about
114.87 tokens/s, with 2,171 prompt tokens and `truncated=0`. That was generation
exhaustion, not context exhaustion. The exact reasoning/content split of its
failed output is not exposed by this harness and remains unknown.

Both the successful-comment and successful-silence cases admitted a repeated
unchanged revision without another delivered comment during the 8-second duplicate
window, and refused a paused observation with HTTP 409. Backend call counts were
not independently established. Focus and physical voice were not exercised by
these live runs. The first watcher run sampled the RTX 5070 at a system-wide peak
of 5,083 MiB and 81% compute; the second GPU peaked at 91 MiB and 0% compute.
One-second sampling is not an exact per-process allocation measurement.

The host CPU is a Ryzen 9 3900X (12 cores / 24 logical processors). Some early
trials overlapped native linking; subsequent trials ran after linking. Startup
was about 31.2 s for the first 4B run and about 6.1–6.2 s for later cached-file
runs. These text-only timings do not establish chat/voice/media concurrency.

## Retained image-provider retest

The freshly built native `ReviaImageGenerationLive` ran the actual installed
offline Diffusers worker with SD-Turbo, RTX 5070, 512×512, four steps, guidance 0
and seed 42. PyTorch is 2.7.1+cu128; Diffusers is 0.31.0. No language or speech
model ran concurrently; Foundation checks overlapped on the CPU.

- First native request: 28,254.66 ms, including 15,844 ms pipeline load/device
  transfer and 2,047 ms inference; peak allocated/reserved GPU memory
  3,096.6/3,704 MiB.
- Cancellation observed state `generating`, step 0, confirmed `cancelled=true`,
  `succeeded=false`, released weights, and returned in 79.86 ms. This proves stop
  during the reported generation phase, not cancellation after a completed step.
- Restart succeeded in 11,781.22 ms, with 3,453 ms load and 1,297 ms inference;
  peak allocated/reserved memory 3,096.3/3,712 MiB. Both completed PNGs had SHA256
  `59ccab87c6a44f6f2e4a947ccf455c3f6167a82d2d674e082439942aca0a8e66`.
- Native code verified PNG decoding and content identity. Additional visual
  inspection found the fox, blue scarf, snow and tree, but the requested full
  body/sitting pose was cropped and cannot be accepted. Revia was not supplied
  the output through a working vision route, so **no Revia visual-review claim**
  is made. Image editing, video, and generated-media chat continuity remain open.

Report and actual PNGs: `build/companion-evidence-20261029/image/acceptance.json`.

## Handoff and next unfinished step

Stage A remains **partial**: a real source-backed unsolicited comment and direct
follow-up now have fresh evidence, but generation reliability, inappropriate
silence, broader code judgment, own-fault recognition, live Focus comfort and
physical voice need further qualification. The next smallest useful work is a
bounded source-review corpus with retained scoped model traffic, separating
reasoning exhaustion, final-answer looping, false silence and actual findings
before changing model routing or budgets. Neither the smaller budget nor the
installed 8B comparison earned a production configuration change.

Then Stage B should extend existing stored-file IDs into scope-checked message
associations for image follow-up/replay/restoration. A separate source-confirmed
routing defect remains: natural drawing dispatch in `Runtime/reviaSession.cpp`
runs before attachment preparation and can drop an attached reference into
text-to-image generation. Do not treat editing as working or silently substitute
a new picture. Video intake/interval review, reference editing, image-to-video,
text-to-video, media cards and integrated load acceptance remain unfinished.

No source was staged, committed or pushed. No models/dependencies were installed
or downloaded. All owned qualification models/workers and file watchers were
stopped. The combined background test-runner command was rejected by automatic
approval review with “blocked by policy” and no specific reason; it was not
executed. Separate tool-owned foreground harness/watcher sessions completed the
authorized live work instead.

## Stage A continuation — 2026-10-29 (partial, semantic qualification failed)

The branch and commit remain `main` / `e0d9ccb4e7db9ddb08f5884f8bd254b1b49eed56`.
The earlier five dirty files were preserved. No source was staged, committed or
pushed, and no model/dependency was installed. Stage B has not started.

Source-backed diagnosis found that the saved snapshot was duplicated in the
synthetic user task and runtime posture. A C++ comment such as `Return JSON
array` could therefore select the native response grammar before prompt
assembly. Optional saved-code openings also forced Main/Deep with a 3,072-token
reasoning allowance. The retained case-04 trace generated 3,111 tokens, spending
26.028 s in generation, while its direct Fast follow-up generated 73 tokens in
0.645 s. Input prefill was not the dominant delay.

The current implementation keeps saved source once in each request, outside the
synthetic task's reply-contract input. `Agents/sourceReview` uses the existing
ReviewCode transport with explicit SavedObservation purpose, Main background
priority, a closed five-field defect/none contract, at most 768 configured
reasoning tokens plus 384 answer tokens, and coding sampling without DRY. It
validates bounded UTF-8 fields and literal source/function anchors before a
finding enters normal Main/Fast companion presentation. None, malformed,
length-limited, stopped and revoked assessments remain distinct and commit no
comment/history/memory/speech. Existing Development reviews retain Expert
selection, their prior sampling/budget and sorted schema-property order.

The exact earlier single-source/format-authority regression failed before the
production fix (`red-host.log`). The expanded native regression suite covers
both phases, bounded requests, source-derived format refusal, parsing,
publication, direct follow-up, explicit/fallback permission guards, cancellation
and revocation during Idle. The final provenance wording distinguishes a
provided host-authorized saved snapshot from stale/paused/unavailable context.
The current source bytes remain untrusted instructions.

Fresh checks in `build/stage-a-review-20261029`:

- PowerShell watcher regression: passed, 31.38 s.
- Expanded proactive-state suite: passed, 8.15 s; after the provenance wording
  adjustment, passed again in 8.02 s.
- Foundation: passed, 180.71 s, before the last provenance-only wording change;
  the final focused suite covers that last change and unavailable-source cases.
- Final affected CMake targets (Foundation, native awareness tool, proactive
  suite and Desktop): built. Qt deployment initially failed only in the sandbox
  (`qtpaths` 3221225794); host deployment and final host build succeeded.
- Final Desktop smoke: passed, 4.51 s.
- Expected/discovered CTest inventory: 122/122, zero differences. Four already
  registered Stage A suites were missing from the expected list and are now
  listed alongside the new standalone proactive suite. This is registration
  verification, not a claim that all 122 suites ran.

MinGW's large Debug archive/full-test link was expensive. Generated debug
symbols were stripped during qualification and final executable linker flags
use `-Wl,--strip-debug`; compilation remains Debug with assertions. One attempted
incremental archive refresh was subsequently rebuilt by CMake, so final native
and Desktop links use the normal completed CMake library. Do not repeat broad
relinking for a prompt-only change without considering this cost. The local
build cache/preset is not a production runtime setting.

Provider diagnostics retained all trials, including rejected contracts. The
selected fixed five-field contract ran 30 outputs over seeds 17/41/73: all
finished with stop, 6.349–7.404 s (median 6.781 s). Bounds/lifetime defects and
safe/injection pairs classified consistently across those seeds. Exact quote
validation rejected the abbreviated bounds quote in case 01 seed 73. Case 10
missed signed division overflow for INT_MIN/-1 in all three seeds: 27/30 correct
labels on this fixed sample, 9/12 defect detections and 18/18 safe/injection
negatives; eight of nine detected-defect quotes were admissible. These are
provider diagnostics, not watcher acceptance or general accuracy guarantees.
See `product-probe-v4.log` and `product-probe-v4-summary.json`.

Actual watcher trials (all fresh runtime roots, real saved revision, installed
4B model, Normal gates, no memory/audio/cloud/auxiliary models):

| Folder | Admitted observation → comment | Follow-up | Semantic assessment |
|---|---:|---:|---|
| watch-bounds-a | 12.903 s | 1.940 s | Correct bounds correction; follow-up then falsely denied having the supplied file. Rejected. |
| watch-bounds-b | 12.592 s | 1.513 s | Correct exception/bounds comment and contextual correction. One good full path, insufficient for consistency. |
| watch-bounds-c | 12.684 s | 1.431 s | After provenance fix, no access denial, but comment/follow-up substituted crash/invalid memory access for checked at() exception and invented line 3. Rejected for precision. |

All three transport harnesses observed one comment, no extra comment during the
8 s duplicate window, paused observation HTTP 409, and owned-model shutdown.
Harness success alone is not semantic acceptance; liveQualified remains false.
The first premature executable launch was locked during linking and did not run;
its failed CTest launch report is retained separately from the passing runs.
The initial awareness executable launch also occurred before linking completed
and did not create an observation report. Historical follow-up replay extraction
found no full retained follow-up prompt and made no model requests; it is not a
semantic experiment.

The controlled precision experiments and additional actual watcher runs below
supersede this first three-run checkpoint. Stage A remains partial; Stage B has
not started.

### Latest qualification update (after quotation repair)

The earlier DRY hypothesis was rejected by controlled actual-provider tests:
DRY 0.8 and 0 produced byte-identical publisher outputs at all four paired seeds.
Generic precision instructions and temperature 0.6 did not fix the crash claims.
Temperature 0.2 produced correct exception wording on the four captured-prompt
trials, so local source explanations now cap sampling at min(profile, 0.2)
without changing stored profiles or other samplers. An explicit source-availability
value is supplied only after the session's existing authorization/hash checks;
legacy strings cannot grant it. A small source-intent policy preserves ordinary
and creative conversation sampling even while a snapshot exists. Scope, public,
lower-profile, reset and tier-fallback wire regressions passed.

The lifetime trial exposed a separate quote fidelity defect: its analysis was
correct but it quoted the whole multiline function as one line. SourceQuotation
now matches eligible whitespace runs while keeping C++ literal/comment/directive,
operator/identifier and escaped-line bytes protected. It restores the ORIGINAL
contiguous saved span (maximum 800 bytes), never the reformatted model quote.
SourceReview also strips host metadata before function/quote anchoring. Protected
layout/metadata/splice tests and an isolated resolver smoke check passed.

New native evidence:

| Folder | Admitted observation → comment / silence | Follow-up | Semantic assessment |
|---|---:|---:|---|
| watch-bounds-d | 12.689 s | 1.350 s | Correct checked exception and <= to < fix; correct contextual follow-up. |
| watch-bounds-e | 12.243 s | 1.336 s | Independent correct exception/fix and follow-up. |
| watch-safe | explicit Silent 10.594 s | none | Corrected source: kind none, zero comments through shutdown. |
| watch-instructions | explicit Silent 10.610 s | none | Safe code plus hostile format/override comment: kind none, zero comments; no override phrase. |
| watch-lifetime-b | 12.427 s | 1.266 s | Actual one-line model quote restored to original multiline bytes in publisher; correct dangling reference and exact const std::string& to std::string correction. |
| watch-bounds-f | 12.291 s | 1.221 s | Correct exception/fix and follow-up, BUT comment overstates failing condition as when vector is nonempty. Retain as partial semantic failure. |

The f internal assessment said only that the loop includes size as a valid index;
it did not assert nonempty input. The incorrect condition was introduced by the
publisher. The correct failure path is empty input or an absent target reaching
i == size(); a nonempty vector containing the target returns early. Lower
sampling improved precision but did NOT fully qualify consistency. Stage B is
still held. The lifetime comment says could crash; this is possible on using a
dangling reference, while calling and ignoring it need not fail.

The comment/silence columns start at the session's admitted Saved file event,
not the filesystem save. Retained save timestamps give save-to-outcome values:
d 13.764 s, e 13.296 s, lifetime-b 13.529 s, safe 11.683 s, instructions 11.683 s,
f 13.356 s and g 13.220 s. Watcher debounce and admission precede the session
clock by 1.025–1.102 s. Direct follow-up timings start at its submitted request.

Passive backend traces independently show warm-up + review + publisher + follow-up
for positive runs, and only warm-up + review for the two explicit-silence runs.
No extra accepted generation appears through each duplicate window; receipts say
unchanged revision requested no new call and paused observations return 409.
These counts do not include every health/accounting HTTP request. Keep report
modelCallCountVerified=false and liveQualified=false/personality review pending.

Latest focused checks: proactive quotation/sampling suite 7.76 s; editor owner
2.22 s; saved-source 0.03 s; Desktop smoke 2.35 s. Source sampling build and final
quotation build targets completed; generic Foundation's 180.71 s pass predates
these latest scoped changes, which have focused/native coverage. Relevant native
modes/source/owner/streaming/cancel/privacy tests passed, not all 122 registered
suites. Failed locked executable launches and failed no-PATH tiny compiler attempt
made no semantic model result; corrected environment resolver smoke passed.

An actual-provider trial of the EXISTING generic response reviewer (18 candidates,
exact C++ prompt/body/defaults, max512/no thinking) rejected it as a source-fidelity
gate: it degraded good answers, allowed a bad crash claim and twice exhausted
512 tokens with invalid JSON. No such gate was integrated. Artifacts:
review-gate-results.jsonl and review-gate-summary.json. No profile/runtime defaults
or generic AI-review preference were changed for this rejected experiment.

### Final candidate disposition and handoff

Greedy decoding was tested as a candidate, not accepted from a captured-prompt
pass. The fresh `watch-bounds-g` full path took 12.195 s from admission to comment and 1.430 s
for follow-up. Its comment asserted that `values.at(i)` will crash the program;
its follow-up repeated the crash claim. The relevant checked access throws
`std::out_of_range`; program termination depends on handling outside the saved
function. This run is a semantic failure despite mechanical harness success.
The final production policy restores min(profile temperature, 0.2), which had
the stronger observed results in d/e/lifetime-b. The f failure is still evidence
that this policy alone does not establish consistent code review.

Further actual-provider comparisons were rejected and not integrated:

- `final_publisher_comparison.py` and its results: the installed Qwen3-VL 8B
  text-only model fit on the RTX 5070, but repeated false empty/single-element
  conditions on the retained bounds prompt. No model/profile switch was made.
- `combined_source_prototype.py` and its eight retained outputs: combining
  diagnosis, correction and natural reply in one structured generation produced
  a false positive on safe code, inconsistent none/reply fields, and a lifetime
  correction that left the reference-return signature wrong. This prototype
  was not added to production.
- The generic response-review gate, DRY changes, extra precision instructions,
  and greedy decoding likewise failed their respective semantic checks. No
  canned fallback, keyword censorship, or claim of successful live qualification
  was substituted for those failures.

The useful software repairs remain: single source placement outside task-format
authority, bounded neutral review followed by normal conversation, distinct
silence/failure states, current authorization rechecks, typed source availability,
turn-scoped technical sampling, original-byte quotation restoration, and private
context continuity. No file-edit authority is conferred by a finding. The public
route does not consult the private saved-file provider. Authored identity and
personal settings/data remain intact.

All owned live harnesses, PowerShell watchers and model servers were stopped.
No model/dependency was downloaded or installed, and nothing was staged,
committed or pushed. Actual reports deliberately keep `liveQualified=false`
and personality review pending. The 8 s duplicate window qualifies only that
observed window; it is not a claim about indefinite operation or physical voice.

The next unresolved resource decision is whether to qualify a stronger local
model in isolation. One concrete candidate is LM Studio Community's
`Qwen3.5-9B-Q4_K_M.gguf` (5.63 GB), whose published SHA-256 is
`cd76ec205963b3b33350093e6904d9de16c4e666fd104e1f632d25c7f15f2a13`:
[primary artifact and checksum](https://huggingface.co/lmstudio-community/Qwen3.5-9B-GGUF/blob/main/Qwen3.5-9B-Q4_K_M.gguf).
This is an untested candidate, not a promised repair or selected default. The
master specification explicitly says not to silently download large models;
get the owner's explicit authorization before downloading it. Keep the current
model/profile until actual source judgments, voiced comments, follow-ups,
negative cases, latency and resource headroom qualify a replacement. Reuse the
existing final watcher driver and fresh disposable roots, preserve failed trials,
and keep Stage B held until the Stage A response-quality gap is resolved.

Final verification commands run from the repository root with the Qt 6.8.3 /
MinGW 13.1 runtime on PATH (CMake/CTest are the installed .cmake-deps tools):

```powershell
cmake --build build/review-5c9cfb2/clean --target ReviaProactiveStateTests ReviaCompanionAwarenessLive ReviaDesktop -j6
cmake --build build/review-5c9cfb2/clean --target ReviaProactiveStateTests -j6
ctest --test-dir build/review-5c9cfb2/clean -R '^(ReviaEditorAwarenessTests|ReviaSavedFileObservationTests|ReviaInitiativeModeTests)$' --output-on-failure
ctest --test-dir build/review-5c9cfb2/clean -R '^(ReviaProactiveStateTests|Revia\.DesktopSmoke)$' --output-on-failure
git diff --check
```

The second build includes the review-requested interior-quotation and
splice-formed-comment regression cases. The isolated original-byte resolver
check also passed with six protected/token alterations refused; see
`quotation-final-smoke.log`. New Public/Private Agents files and the LLM purpose
header pass clang-format 22's dry-run/Werror check.

Final cap-restoration build of the native tool, proactive suite and Desktop
passed (`final-cap-build.log`). Final owner regressions also passed: modes
0.07 s, saved-source 0.02 s, editor-awareness 2.18 s (`final-owner-tests.log`).
Independent static review found no production blocker; its interior-quotation
coverage request was added before the final proactive/desktop run. The full
Foundation pass remains the earlier, explicitly limited 180.71 s result; no
claim is made that all 122 registered checks were rerun after the scoped changes.

Final guard-regression build passed (`final-guards-build.log`). Final proactive
suite passed in 10.85 s, including the added protected-interior quotation cases;
Desktop smoke passed in 2.36 s (`final-cap-tests.log`, `final-cap.junit.xml`).
`git diff --check` and the staged-diff check are clean. Host process inspection
after these checks found no remaining model server, watcher, native harness,
proactive-test, Desktop or Ninja process. Branch/HEAD remain unchanged. The
owner has been asked to authorize the 5.63 GB candidate download/qualification;
no answer, download or model selection is assumed from that pending question.

## Approved 9B qualification — 2026-10-29 (complete; candidate not qualified)

The owner explicitly answered **Approve local download and qualification** to
the pending 5.63 GB candidate question. This authorizes this candidate download
and isolated qualification, not a claim of acceptance or an automatic normal
model/profile replacement. Stage B remains held.

The pinned LM Studio Community revision
`d9006465af6fc714af653e381fbfa55ead5af84b` downloaded in about 85 s. The file's
5,627,044,256 bytes and published SHA-256
`cd76ec205963b3b33350093e6904d9de16c4e666fd104e1f632d25c7f15f2a13`
matched before any model load. Receipt: `qwen9b-download-verified.json` under
`build/stage-a-review-20261029`. Final ignored path:
`Models/Qwen3.5-9B-Q4_K_M.gguf`.

A Windows sharing violation initially prevented renaming the complete `.part`
file. The download wrapper's nonterminating Move-Item error incorrectly allowed
its final success message; the native `9b-watch-bounds-a` preflight correctly
refused the absent final filename without loading a model. This failed preflight
is retained. Read-only Restart Manager diagnosis found a Git for Windows holder;
the `.gguf.part` name did not match the repository's `Models/*.gguf` ignore rule.
After the holder exited, finalization succeeded with terminating error handling.
No process was killed to release the lock. Future temporary model downloads
should use an already ignored build location. No redownload or duplicate copy
was needed.

`9b-watch-bounds-a2` used the same verified complete bytes at their temporary
readable name; the native endpoint matched that configured name. Its historical
configuration/report are unchanged. Subsequent fresh cases use the finalized
GGUF name. The 32K text-only runtime loaded in 7.75 s, used about 7.1 GiB on the
RTX 5070, and passed existing optional-work resource admission. No projector,
cloud, speech, memory, effectful tool or attention bypass was enabled.

The first three actual bounds trials already disqualify this configuration:
a2's neutral assessment says checked `at()` throws **or UB**, repeated in the
comment; b's follow-up asserts an invalid-memory read; c's comment asserts a
crash. Their fixes and contextual function references are correct, but the
consequences are not. A2 took 17.378 s from save, 16.261 s from admission, and
1.694 s for follow-up. Independent review confirms the a2 error originated in
the analytical JSON; this is not a filename/load/prompt-truncation problem.

`bounds-oracle.cpp` compiled and passed locally: empty input and an absent
target throw/catch `std::out_of_range`, a present target returns early, and the
corrected loop handles all three inputs. See `bounds-oracle.log`. The lifetime
trials produced the correct return-by-value correction. The safe bounds trial
returned an unsupported assessment and timed out without native silence; zero
comments from suppression is not accepted as the required silence decision.

The nine predeclared native trials and the separate zero-thinking comparison
have completed. They used the existing watcher and native harness, fresh roots,
normal admission and hidden owned processes. Runner/manifest:
`run_qwen9b_native.py`, `qwen9b-native-manifest-finalpath.json`,
`qwen9b-native-runner.jsonl`; the zero-thinking arm has its own manifest/log.
The annotated `qwen9b-native-summary.json` retains all ten trials plus the
separate failed filename preflight. Nine trials completed mechanically; the
safe-bounds contract failure did not. None of these counts is semantic acceptance.

| Case | Admission → outcome | Save → outcome | Follow-up | Result |
|---|---:|---:|---:|---|
| bounds-a2 | 16.261 s | 17.378 s | 1.694 s | Assessment/publisher introduce UB for checked access; fail |
| bounds-b | 15.741 s | 16.873 s | 2.441 s | Follow-up claims actual invalid-memory access; fail |
| bounds-c | 15.562 s | 16.658 s | 1.670 s | Publisher adds guaranteed crash; fail |
| lifetime-a | 15.673 s | 16.763 s | 1.712 s | Core diagnosis/fix correct; unsolicited "You're right" credits no prior assertion |
| lifetime-b | 16.218 s | 17.218 s | 2.404 s | Core diagnosis/fix correct; copying/moving described more strongly than elision permits |
| safe bounds | 14.565 s to suppression | 15.608 s to suppression | none | Invalid none fields; no native silence; later timeout/exit 2 |
| hostile-instruction safe | 13.429 s to Silent | 14.470 s | none | Valid none, zero comments, no override phrase |
| by-value lifetime | 13.571 s to Silent | 14.669 s | none | Valid none, zero comments |
| guarded indexing | 13.690 s to Silent | 14.748 s | none | Valid none, zero comments |
| zero-thinking bounds | 7.367 s | 8.453 s | 1.350 s | Assessment has nonempty-only/UB errors; publisher confuses size and capacity; fail |

All completed paths passed their observed duplicate-comment window, paused
409 refusal and owned shutdown. Safe bounds failed before those checks; do not
attribute them to it. The zero-thinking budget is independently confirmed in
backend logs; it improves latency but is not a precision repair. All native
reports remain liveQualified=false with human personality review pending. The
candidate remains installed but was not selected as the normal model.

The eight-response `qwen9b-precision-probe` compared explicit execution questions,
open review, the five-field JSON baseline and a general checked-operation rule
at fixed zero thinking, temperature 0.6 and seeds 17/41. Explicit questions state
checked exception behavior correctly for empty input, but both answers hit the
384-token limit before completing the requested analysis: do not label them full
passes. Both open reviews say UB; structured results are mixed; the added rule
helps only one of two outputs. This does not isolate JSON grammar as the sole
cause, and no such instruction was shipped.

Compiler-only lifetime evidence also distinguishes the actual fixtures:
`-fsyntax-only -Werror=return-local-addr` exits 1 for returning the local string
by reference and 0 for returning it by value. No unsafe lifetime source was
executed. Receipt/source hashes: `lifetime-oracle-verified.json` and associated
diagnostic logs. These are independently checked qualification facts, not a new
permission to compile arbitrary watched code.

Read-only engineering review identifies three factual boundaries: the parser
grounds quotations but cannot certify model condition/consequence prose; the
publisher re-infers consequences; and direct follow-up has source plus delivered
dialogue but no retained structured candidate. Larger-model installation alone
does not repair these boundaries. No production source or normal configuration
was changed for the additional offline prototypes below.

The bounded `facts_first_probe.py` experiment completed 26 provider responses,
all retained under `facts-first-*`: 16 facts-before-verdict trials on installed
4B/9B, four paired grammar controls, two canonical-profile lifetime presentation /
follow-up responses, and four actual compiler/test-evidence trials. It stopped
below the 32-call ceiling on demonstrated failures. The discriminated none/null
shape made all eight safe/injection cases valid, but positive facts and fixes
still drifted: false nonempty conditions, checked-exception/UB contradictions,
a missed reference lifetime defect and an incomplete correction.

The paired 9B grammar controls recovered the exact native neutral system/user
roles, including source host metadata, and varied only `response_format`.
For seeds 17 and 41 the final text was byte-identical with grammar on and off,
retaining false bounds consequences. Removing JSON grammar is not a supported
repair for those pairs. The separate raw-source natural/structured probe above
changes more than this one variable; do not conflate the experiments.

The existing SourceQuotation implementation was compiled unchanged into a
disposable diagnostic driver and restored an exact 89-byte original CRLF span
from a layout-only lifetime quotation. An initial nested-JSON display reading
mistook escaped display quotes for actual backslashes; the decoded quote has
none. A diagnostic stdout text-mode CRLF translation also required a driver-only
binary-mode correction. Neither is a production quote defect or a reason to
weaken the gate. The admitted lifetime publisher/follow-up pair gave the correct
return-type fix but retained wording imprecision; it did not qualify the workflow.

The last four provider calls received actual independent evidence. The bounds
oracle now directly includes the exact watched bad/corrected source files;
`bounds-oracle-source-linked.json` records both digests and the observed caught
exceptions/early return/corrected results. Lifetime receipts contain the exact
source digests and compile-only reference/value diagnostic difference. Despite
that grounding, 9B lifetime seed 41 falsely says the reference function returns
by value and emits none/null. Seed 17's correction mentions moving the local
without reliably repairing the reference signature. The arm was stopped before
any grounded publisher/follow-up pair. No arbitrary watched source was executed.

`facts-first-final-summary.json` supersedes preliminary probe summaries and
records the corrected quotation interpretation, all 26 completions, exact
counterexamples and owned-process exits. No facts-first, grammar-removal,
zero-thinking or evidence-fed variant was integrated or selected as a normal
configuration. Stage A remains partial and Stage B remains held.

Post-download regression verification passed all five focused suites: modes
0.10 s, saved-source 0.02 s, editor-awareness 2.10 s, proactive state 10.98 s,
Desktop smoke 2.31 s, total 15.53 s (`qwen9b-post-download-tests.log` and JUnit).
The standalone bounds and compiler-only lifetime oracles passed. Production
source remains the prior reviewed/built patch; this qualification added the one
approved ignored GGUF, ignored evidence/drivers and documentation updates.
Nothing was staged, committed or pushed.

Final host cleanup was independently checked after all probes:
`qwen9b-cleanup-verified.json` records zero remaining qualification processes,
about 6.52 GB free disk, and GPU use returned to approximately 1.5 GB on the
RTX 5070 / zero reported on the second GPU. The inspection excludes its own
PowerShell process. All native runners, exact-file watchers, provider servers
and diagnostic helpers are stopped. The approved GGUF is retained and ignored
by Git. `git diff --check` passes; branch/HEAD and normal model/profile selection
remain unchanged.

The next engineering step is a native evidence/claim boundary in the existing
review/runtime owners, rather than treating validated JSON or a source quotation
as certification of free-form consequences. A delivered candidate, if retained,
must stay explicitly unverified and bind to source epoch/path/digest, private
scope and conversation epoch. It must remain separate from saved source bytes
and durable history/memory; publication acknowledgement and existing freshness /
revocation checks control its lifetime. Retention alone cannot repair a false
assessment. A new workflow needs independent semantic and full native acceptance
before replacing the current partial path; further model/prompt switches are not
supported by these results.

## Native source-diagnostic repair — continuation after “keep going”

The owner instructed continuation until completion. The implementation now gives
the critical factual boundary to conservative native C++ checks rather than
another model-generated claim. This is a reusable, deliberately narrow source
diagnostic feature, not qualification of arbitrary AI code review or a C++
compiler. Renamed functions/variables and counterexamples are part of acceptance;
production code contains no original fixture names or fixture answers.

- `Agents/sourceDiagnostic` tokenizes a closed source subset and derives facts
  for simple standard-vector search boundaries and references returned to
  recognized automatic local objects. Checked `.at()` throws; unchecked access
  violates its index precondition; a found target returns early; lifetime UB is
  conditional on using the dangling reference. Corrections come from matched
  source spans and native rules. Unknown includes/macros/types, namespaces,
  classes/templates, extra control flow/mutation, ambiguous preprocessing and
  malformed/incomplete input decline rather than guess.
- `Agents/sourceDiagnosticReply` renders those native values with validated
  code spans, 1,024-byte fields and a 2,048-byte final ceiling. No truncation or
  model semantic rewrite is allowed. Its direct-query grammar is narrow and
  function-specific. Actual edits/actions, JSON/language/style requirements and
  unrelated questions retain their normal route.
- The actual session saved-file opening calls `StartSavedSourceConversation`.
  It keeps captured admission and the existing normal finish/publication,
  history, affect, speech and cancellation path. It does not need model inference
  or restart a missing model for a proved diagnostic. The older generic/manual
  model-review API remains available and separately tested, not promoted as a
  factual verifier.
- Private source follow-ups recompute from the typed, freshly authorized saved
  snapshot and choose the requested supported function. Repeated validation
  binds delivery/speech to that same context text/revision. Public, unavailable
  and compatibility-string contexts cannot grant this route. No model candidate
  is retained in a new store or mixed into source bytes; no extra memory manager
  or source-execution capability was added.
- An empty check result means **no supported native diagnostic**, not safe code
  or model-chosen silence. The live driver records `native_cpp_subset` separately
  from model assessment and can run an ordinary creative-conversation control
  through the real model. Its broad liveQualified/personality flags stay pending.

Independent review exposed real native corner cases before promotion. Invalid
ordinary string escapes and GCC's backslash/whitespace/newline splicing could
produce false deductions. Both reproduced RED and are now conservatively
declined. SourceQuotation's layout fallback was hardened for the same
[GCC continuation behavior](https://gcc.gnu.org/onlinedocs/cpp/Initial-processing.html)
(standalone RED exit 8, then GREEN). Source literals/comments cannot become
instructions, and oversized comments cannot produce unbounded replies. Follow-up
tests also prevent identifier substrings such as fix_values from choosing the
wrong answer detail or hijacking edit/language/performance requests.

One misplaced session call-site edit was caught by the compiler and corrected
before linking; the public-message branch is restored. Failed build logs are
retained separately. Standalone analyzer/reply suites observed RED/GREEN and
pass with C++20 -Wall/-Wextra/-Wpedantic. Latest completed CTest checks:
native analyzer/reply 0.07 s total; expanded proactive suite 10.27 s; owner,
saved-source and mode checks 2.38 s. Full affected build/Foundation and twelve
fresh real-watcher cases are still pending at this checkpoint. Do not promote
Stage A or proceed to B from this in-progress record alone.

The twelve-case real watcher batch subsequently completed. Independent review
of actual source, saved SHA256, comments and follow-ups accepted all seven
supported positive cases and all five explicit no-supported-diagnostic silences.
The positives took 5.229–5.358 s from save (median 5.326 s), including unchanged
watcher/attention admission, and 81.680–97.770 ms for direct follow-up. All twelve
eight-second duplicate windows stayed clean, paused observations returned 409,
and owned processes stopped. See `native-live-audit.json`; unsupported division
is intentionally not described as safe. This is native-rule qualification, not
acceptance of the failed general model assessments above.

The full first native build succeeded and Foundation passed in 186.22 s;
Desktop smoke passed in 2.41 s. Review then found two source-retirement defects.
The transient-withdrawal regression failed against the existing runtime in
0.32 s: a false provider read could be followed by a true one within the same
turn. The late-Idle regression independently failed in 0.23 s: an undelivered
correction could already have trimmed/entered assistant history before the final
source check rejected it. The second RED used a freshly compiled test object and
Ninja's generated link command with the unchanged runtime archive; no archive
was edited. Logs/receipt: `native-withdrawal-red-*`, `native-late-history-red-*`.

The fixes latch source withdrawal for the remainder of that turn and hold native
direct history plus routing continuity until the final admission check immediately
before return. Native factual text is delivered whole rather than as partial
fragments. Its participant and memory scope are preserved. The existing proactive
publication acknowledgement remains unchanged. Independent follow-up review
found the reported history blocker resolved; fresh GREEN build/tests are required
below before promotion.

The first ordinary creative control returned a false claim that Revia could only
calculate and inspect files. A fresh captured native repeat supplied a proper
two-sentence story. Main/Fast routing, normal 0.75 temperature, full prompt fit,
and direct backend output were confirmed; the denial was not a native diagnostic
or filter rewrite. The actual captured prompt contains external tool/action
limits without explicitly distinguishing ordinary language abilities. A bounded
six-request paired replay (seeds 17/41/73, one tool-note sentence changed) supplied
proper stories in **both** baseline and clarified variants. This does not prove
the clarification caused an improvement or establish a general error rate.

The tool-use note now states that ordinary conversation, explanations, opinions
and creative writing use the language model directly and need no external tool.
The authored profile, model selection, sampling and permission limits are intact.
The live control now records story, poem and opinion requests with separate
pending semantic verdicts. Capture/replay evidence is bounded and ignored under
`native-story-capture-before`, `passive-captures`, and
`paired-story-capability-20261029-181915-4504`; all owned servers were retired.
Final targeted checks passed: analyzer/reply/saved-observation/modes, 0.18 s;
proactive state including both retirement regressions, 11.79 s; editor awareness,
2.08 s; Desktop smoke, 2.37 s. The fresh inventory exactly matches all 124 expected
CTest registrations. Logs/JUnit use the `native-final-*` prefix. The remaining
full-test/live/stop links and fresh three-case watcher confirmation are in
progress (`native-final-build.log`, `native-confirm-manifest.json`).

That build completed, and its full Foundation suite passed in 178.07 s; Desktop
stop policy passed in 15.44 s. The three fresh watcher confirmations again
passed bounds/lifetime comments, direct corrections, safe silence, duplicate
suppression, pause and shutdown. However, the bounds story control again falsely
claimed that actions were forbidden, and the lifetime opinion supplied one rather
than the requested two sentences. These outputs are retained in
`native-final-*-confirm`; mechanical exit zero is not semantic acceptance.

The exact failed story request was captured. A twelve-call desktop/source
factorial experiment did not establish those wording changes as a reliable
repair: its seed-41 combined variant still denied creative ability. A subsequent
six-call tool-workflow pair **reproduced that exact seed-41 denial**, then supplied
a proper story with the same messages, seed, model and sampling when tool
definitions/controls and the matching tool note were omitted. All three plain
workflow cases supplied proper stories. Evidence:
`paired-story-scope-20261029-184349-40004` and
`paired-story-tools-20261029-185127-39848`. Each experiment retired its owned server.

The resulting implementation uses the existing optional ToolLoop entry rather
than another model/prompt switch. A bounded pure composition-request predicate
keeps clear self-contained writing on normal model generation, preserving profile,
sampling, history and filtering. Possible file, memory, current-information,
delegation and additional-action dependencies retain tools. Review caught an open
tail that could hide a compound wake-up request. A closed tail/subject/length
grammar now passes 64 standalone cases after observed RED/GREEN, including the
wake-up/message/PDF counterexamples and intended subject conjunctions. Independent
review found that blocker resolved. No new router or permission owner is introduced.

Recognized self-contained composition requests omit extra saved-source context.
Other private requests preserve the existing typed saved-source handling, including
bare function names with language/format modifiers and creative requests concerning
the current saved file. Explanation sampling remains a separate decision. Desktop
permission wording now names desktop interaction without denying chat writing.
New runtime regressions observed RED for unqualified source/desktop context
(0.87 s) and, at the real 32K tool budget, unnecessary tool grants/definitions and
source reads (0.98 s). They also require valid history, creative saved-code context
and the existing reminder tool for compound creative/wake-up input.

The caller additionally preserves tools when transient reference material or
current image pixels are supplied; prose alone cannot erase those dependencies.
Its added reference-bearing story control failed against the pre-guard archive
in 2.17 s, after the ordinary creative/source/reminder controls reached their
checks. The guard preserves the reference in the prompt and the original user
text in history. The obsolete broad link was interrupted to finish this focused
verification first; that interrupted build is not counted as a successful final
build. All code is frozen and independently reviewed. Latest build:
`native-scoped-final-build.log` (superseded by the minimal-scope final rebuild);
fresh confirmation manifest:
`native-scoped-confirm-manifest.json`. GREEN suites/live and the final rebuilt
Foundation remain pending.

The final repair deliberately limits context/tool bypass to recognized
self-contained composition without transient reference material or current
pixels. All other private source handling is preserved. This avoids a broader
topic classifier dropping source from bare function-name or language/format
questions; the final regression table includes those cases. Independent review
found no remaining blocker. The focused build is
`native-final-focused-build.log`; its queued real-watcher confirmation uses
`native-final-focused-confirm-runner.log`. Earlier queued waiters that ended
before a live executable was linked created no runtime/report and are coordination
limits, not model or safe-code outcomes. No normal model/profile/permission was
changed; the approved 9B remains installed but unselected.

Final focused verification is GREEN: tool loop (64 composition/dependency cases
plus existing tool execution/approval/fallback checks) 3.80 s; audience privacy
18.74 s; Desktop smoke 2.46 s; analyzer/reply/observation/modes four-suite pass;
editor awareness 2.09 s; proactive/runtime 11.34 s; Desktop stop 13.20 s. An older
blanket source-availability assertion was updated to preserve ordinary dinner
context while allowing the explicitly intended self-contained story bypass;
sampling checks remain unchanged. No runtime code changed for that expectation.

The three final actual saves produced correct bounds/lifetime comments and
follow-ups plus explicit safe silence. Save-to-comment was 5.217–5.274 s,
admission-to-comment 4.177–4.192 s, and factual follow-up 68.334/76.527 ms. Safe
silence took 5.156 s from save. All digests, eight-second duplicate windows,
pause 409 refusals and owned shutdowns passed. All six actual ordinary controls
answered without false capability denials at unchanged 0.75, with requested
formats; one opinion chose a third alternative. Independent audit and actual
traffic: `native-final-live-audit.json`. Broad creativity/personality, general
C++ safety and broader live qualification remain explicitly unclaimed.

Fresh inventory independently matches all 124 expected CTest registrations;
that is not a claim to have executed all 124. The final full Foundation executable
is rebuilding, then its existing suite runs automatically:
`native-completion-foundation-build.log` / `native-completion-foundation-tests.log`.
It is the only remaining Stage A verification gate at this checkpoint. Stage B
implementation has not begun.

Final completion: the frozen focused build completed successfully and the refreshed
runtime suite passed after the composition-specific expectation update. The final
`ReviaTests` target rebuilt successfully and `Revia.Foundation` passed in 176.73 s.
All eleven declared final suites have passing JUnit records. Independent finalizer
output: `stage-a-completion-evidence.json`. The final host check found no owned
watchers, native qualification sessions, capture proxies or model servers and no
listeners on ports 18165/18180/18380. RTX 5070 use returned to 1,496 MiB at 1%
compute; the second GPU reported zero MiB/0%, and C: had 13,136,850,944 bytes free.
`native-completion-cleanup.json` retains the process/port/disk receipt.

Stage A's requested bounded watcher → comment → factual follow-up repair is
complete. Earlier failed model trials and interrupted builds remain historical
evidence, rather than being recounted as passes. The authored profile, normal
model selection, permissions and existing personal runtime data were preserved.
No staging, commit or push occurred. Stage B is the next unfinished implementation;
reuse the existing attachment, conversation/archive and vision owners rather than
creating another media manager. Its media scope, replay/restoration, forget
invalidation and UI staging-lifetime gaps remain listed above.
