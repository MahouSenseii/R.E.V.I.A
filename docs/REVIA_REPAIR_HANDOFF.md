# Revia repair implementation handoff

Production repairs, local verification and paired evaluation are complete. Hosted verification exposed a cue-fixture port reservation failure and a resource-sampling identity issue. Both verification-only repairs now pass targeted and independent Foundation/driver checks; a new exact-commit complete gate is pending. No grade increase is claimed.

## Exact identities

- Audited base: `44aa6bc4d890fe050a4336c530b340d08a3a0767`.
- Implementation and paired-evaluation baseline: `ad207d6f04aad2a6981fa2a26a8f35124f287c0f`.
- Frozen production candidate and first evaluation: **`58c905de45d0146a84c3f5e040e34f049a658380`**.
- Previous code/evaluation checkpoint: **`f20193bc0a414df81bcb4cf82a0815053baaecb6`**. Six verification files changed; all 659 production file hashes remain identical.
- Candidate branch: `codex/revia-audit-repairs`; worktree: `C:/Users/davis/.codex/worktrees/revia-audit-repairs/R.E.V.I.A`.
- Baseline evaluation worktree: `C:/Users/davis/.codex/worktrees/revia-repair-baseline/R.E.V.I.A`. Production remains at the baseline; the identical six-file evaluation overlay is explicitly captured as dirty source.
- Retained pre-follow-up GitHub run: https://github.com/MahouSenseii/R.E.V.I.A/actions/runs/37977328343 (build/relay passed, 92/93 native tests passed; cue bind failure retained).
- Final code review binding: `build/repair-evidence/independent-review/f20193-review-binding.json`. The first model outcomes remain attributed to `58c905de`; the separately retained repetition is attributed to `f20193bc`. A later handoff-only commit changes documentation, not tested code.

No earlier repair candidate was found in inspected branches, worktrees or implementation records. The available brief was the attached R1–R9 repair prompt (SHA256 `b5c751694e5e3c96aaa66846782ec2a758b6a3c1273ca529d172ffa5bc8ea62e`), the retained independent review, and the explicit R10/R11 implementation request. No separate newer prompt was located. The primary checkout's tracked files, user data, unrelated worktrees and older branch were preserved. Main was not merged as part of this candidate handoff.

## Ownership and review

Root integrated the candidate and owned R6/R8/R9. The policy worker owned R2–R4, the scope worker R5/R11, and the build worker R1/R7/R10. File boundaries and the plan are in `docs/superpowers/plans/2026-10-09-audit-repairs.md`.

A fresh independent agent inspected the actual diff and production callers, reproduced three additional search/continuity findings, and independently reran their final acceptance controls. All three production findings were closed. The reviewer also independently ran the journal, scope, desktop, build-driver, Windows operator and CLI loader checks; inspected raw model outcomes and provenance; and reran the frozen coding oracles. This is an independent agent review, not a claim of an external Claude review.

Review records: `build/repair-evidence/independent-review/{final-review.txt,frozen-review.json,evaluation-review.txt,evaluation-code-checks.json}`. The historical review preserves failed probes and the initial desktop loader timeout caused by the reviewer's incorrect Qt path. The same binary passed under the unchanged timeout with the configured Qt6.8.3 path.

## R1–R11 disposition

| Item | Disposition and evidence | Boundary |
| --- | --- | --- |
| R1 native CI | Repaired build/qualification path. Authenticated prior job log shows cancellation during large Debug archive/link work. Measured bounded concurrency, single archive indexing, Debug `-Og -g1`, fail-closed exact test inventory and artifact retention implemented. | Complete native/hosted outcomes below; no OOM claim. |
| R2 search authority | Fixed, reproduced and independently verified. Authored denials, quotations, fences, later amendments, local/private intent, freshness, conjunction withdrawals and oversized-input limitations have actual callback and provider controls. | Natural-language intent parsing remains bounded heuristic policy. |
| R3 JSON authority | Fixed, reproduced and independently verified. Comma clauses and escaped-quote parity preserve genuine authority. Actual schema, post-review delivery, invalid-answer history and scoped memory gates have negative and positive controls. | Schema validity alone does not establish correct answer values. |
| R4 continuity | Fixed, reproduced and independently verified. Dotted names, numeric-only/multiple suffixes, URLs, relative paths and corrections survive real eviction/archive reopen/final private wire. Original malformed-decimal and sentence-boundary assertions retained. | Real model still sometimes emits wrong/nested output despite receiving the correct token. |
| R5 background scope | Fixed, native regressions reproduced. Original immutable participant/audience/consent/constraints and real cancellation ancestry retained; saved-contract replacements refuse. Sixteen controls pass including genuine successful native writes. | Real-model trials failed before valid dispatch and cannot qualify this guard. |
| R6 persistence reads | Fixed. Checked reads distinguish missing/empty/EOF/error and preserve conservative health. Initial/mid-read and post-durable-write health failures reproduced, repaired and independently checked. Windows and hosted Linux controls pass. | Durable append acknowledgement remains truthful even when the subsequent health read fails. |
| R7 Windows operator | Fixed fixture coverage. Actual script/config paths contain spaces; five Windows tests ran with zero skips locally and in hosted CI. | No unrelated operator permissions changed. |
| R8 branch assessment | Assessed. Existing branch `0b93facf5920cd80721fa9a3929ad6b556201c7f`, merge base `94dbd9c293f7392cf125663b4676a64e735ae43a`; 40 main-only /31 branch-only commits at baseline independently verified. | No merge/deletion or claim of running the old branch's tests. See branch assessment. |
| R9 evaluation | Performed on frozen identities: 52 private answer slots and eight real worker slots retained, plus executable code checks and independent semantic/provenance review. | Partial qualification, not a broad capability pass; seeds' backend application and owner personality remain unverified. |
| R10 CLI package | Fixed. Fresh baseline DesktopOFF CLI loader fails `0xc0000135`; fresh candidate passes Windows-only PATH; removing only the deployed thread DLL fails again. Registered loader gate added. | Loader stops before application entry; this is import resolution, not a full application/model startup claim. |
| R11 desktop stop | Diagnosed and fixed in the fixture. Stopped sessions had caused both sides to refuse. The corrected fixture preserves B allowed/A denied and filesystem assertions, adds stopped controls and requires A's explicit companion-denial reason. | No production permission correction was necessary. |

## Builds and test commands

All paths below are relative to the candidate worktree unless identified otherwise. Compiler: Qt MinGW GCC13.1; Qt6.8.3; CMake/Ninja. Assertions remain enabled (`NDEBUG` absent from all 592 configured compile commands).

```powershell
$env:PATH = 'C:/Users/davis/Qt/Tools/mingw1310_64/bin;' + $env:PATH
cmake -S . -B build/repair-integrated -G Ninja -DCMAKE_BUILD_TYPE=Debug `
  '-DCMAKE_C_FLAGS_DEBUG=-Og -g1' '-DCMAKE_CXX_FLAGS_DEBUG=-Og -g1' `
  -DBUILD_TESTING=ON -DREVIA_REQUIRE_DESKTOP=ON -DREVIA_COMPILE_JOBS=2 -DREVIA_LINK_JOBS=1 `
  -DCMAKE_C_COMPILER=C:/Users/davis/Qt/Tools/mingw1310_64/bin/gcc.exe `
  -DCMAKE_CXX_COMPILER=C:/Users/davis/Qt/Tools/mingw1310_64/bin/g++.exe `
  -DREVIA_QT_ROOT=C:/Users/davis/Qt/6.8.3/mingw_64
./Tools/Build/InvokeMeasuredBuild.ps1 -BuildDirectory build/repair-integrated -Parallel 3 `
  -TimeoutSeconds 2700 -EvidenceDirectory build/repair-evidence/build/frozen-all-targets
./Tools/Build/InvokeCheckedCTest.ps1 -BuildDirectory build/repair-integrated `
  -EvidenceDirectory build/repair-evidence/build/f20193bc-final-ctest
npm run test:operator --prefix Tools/Presence/WebDemo
$env:REVIA_WEB_NATIVE_HOST = "$PWD/build/repair-integrated/ReviaWebGuestHost.exe"
npm run test:native --prefix Tools/Presence/WebDemo
./build/repair-evidence/policy/replay.ps1 -Mode Wire
./build/repair-evidence/journal/journal-health-green.exe read
./build/repair-evidence/journal/journal-health-green.exe all
```

Local fresh configurations additionally set `FETCHCONTENT_SOURCE_DIR_HTTPLIB/JSON/SQLITE/EXPAT` to the pinned dependency source trees beneath the primary checkout's `build/debug/_deps`. No primary build archives were reused. Exact configuration argument arrays, compiler commands, binary hashes and process samples accompany each receipt. Existing evidence directories are immutable records; use new output directories for a reproduction. PowerShell/Git source capture must trust the exact managed worktree; no global trust configuration is required.

The final all-target build passed in **617.884 seconds**, no timeout and unchanged input bytes. Its pre-correction resource sampler recorded working/private samples of **2.905/3.011 GB**, but PID reuse can contaminate that sampler, so these are not qualified build-only peak measurements. The committed no-op refresh passed in **2.508 seconds** with Ninja reporting no work. Native relay **1/1** passed, zero skips, **2.296 seconds**. The first full CTest run on `58c905de` executed 93 tests, passed 92 and failed one (`Revia.BuildDrivers`), with zero skips. The failure was not suppressed: nested PowerShell hashing and repeat-fixture cache handling were repaired in `f20193bc`, and the exact CTest launch passed. Final local code-candidate qualification: **93 expected, 93 discovered, 93 executed, 93 passed, zero skipped/failed**, CTest exit0 and qualification exit0, **334.434 seconds**, trusted source identity. `f20193bc-noop-refresh` passed in2.525 seconds, unchanged source; `f20193bc-native-relay.log` passed1/1, zero skips, in2.170 seconds. Final full result: `build/repair-evidence/build/f20193bc-final-ctest/test-result.json` and JUnit SHA256 `D26B958EFED1D003122FEED33BDB365CF67D6BF81FDBCFDCE62F05BA81E8B7AA`. Hosted native result remains pending in this draft.

The same-input archive probe preserves all 231 members and all archive bytes except symbol-table timestamp: **316.622 to179.369 seconds**. Full-symbol versus optimized representative session links were309.150 versus27.779 seconds, but those binaries also include repair changes, so that comparison is not a flags-only experiment. Assertions and required targets were retained. Driver tests cover empty/missing/extra/skipped/failed inventories, untrusted source identity, interrupted build/log retention, successful qualification and duplicate CMake PATH resolution; nine controls now pass, including real SHA256 hashing when Get-FileHash is unavailable in a nested process. Known-answer hashing passed under PowerShell5.1 and7; repeated fixture directories and the exact CTest launch passed.

## Frozen R9 evidence

Campaign root: `C:/Users/davis/OneDrive/Documents/GitHub/R.E.V.I.A/build/repair-r9-20261009/`.

- Candidate: `candidate/`; baseline: `baseline-private-absolute-path/`. Both have runner exit0, 26/26 recorded slots, zero missing slots, stable source/build/provider receipts and no mismatches. The baseline's original `baseline/` directory retains a launcher failure before any model outcome: a relative build path resolved against the process working directory. The absolute-path retry did not selectively repeat failed answers.
- Exact candidate evaluation binary SHA256: `2e97e92e3a4a42f84517f11343666a9e29d68bf691eabe07f1c68b739cbc827d`; baseline: `aac29cc7d4e853cfc8bd347319aa56e1ac6bad689709d37ff1d5cfc7ff8e8cfb`.
- Model: Qwen3.5-4B Q4_K_M, SHA256 `00fe7986ff5f6b463e62455821146049db6f9313603938a70800d1fb69ef11a4`; llama.cpp build10453/commit3cb7ffb1a; 8192 context, one slot, thinking off, loopback18769, RTX5070. Host also has RTX2070 SUPER and128GiB RAM. Authored Revia profile and all32 configuration file hashes match. Raw settings digests include different absolute worktree paths; relative-path/content equivalence was independently checked.
- Corpus SHA256 `3642f3e9d5049d9f12e922a7ae5f6511f92dd9ce532f51cc8cbde3133bddacea`; code oracle `7a88fd8079ed439656ec85a950e328bc9e62af902e63124e53bda8d4f9dc6b90`; 13 unique designs with repeated seeds101/211. Requests contain those seeds; backend application is not independently established.
- Per-case fresh runtime, real bounded history and archive close/reopen were used. Research replays identical excerpts freshly checked against official Python and CMake sources; this does not establish autonomous live browser research. Expected answers and executable oracles were not given to the model.
- The owned model server was stopped after collection and the reviewer's live identity check. Launch/stop and all request/response receipts remain retained.

| Frozen metric | Baseline | Candidate | Interpretation |
| --- | ---: | ---: | --- |
| Available private answers |26/26|26/26| Availability only. |
| Valid delivered JSON |25/26|26/26| Syntax only. |
| Arithmetic exact expected values |0/6|0/6| Includes wrong arithmetic and unexpected nesting. |
| Targeted exact JSON answers |2/4|4/4| Two designs, each repeated twice. |
| Coding executable oracle |3/4|4/4|216 interval /211 inventory checks per accepted function; not exhaustive correctness. |
| Research exact expected values |2/6|2/6|Other answers nest otherwise supported facts. |
| Continuity exact expected values |0/6|0/6|Correct tokens are frequently nested in an unexpected structure. |
| Completed real worker tasks |0/2|0/2|No native tool dispatched. |
| Qualified live participant-scope trials |0/2|0/2|Both trials per side inconclusive: no valid actionable proposal. |

Independent descriptive semantic inspection, kept separate from the frozen scores: arithmetic correct underlying answer2/6 each; source-supported research content6/6 each; exact continuity token appears2/6 baseline versus5/6 candidate. Candidate filename seed211 emits schema text rather than the filename. An unscored boundary probe also found candidate interval seed101 returns a tuple for tuple input instead of the requested list; the frozen oracle uses list pairs. No post hoc observation replaces the predefined strict score.

All eight worker outcomes are preserved. The original participant211 `passed:true` fields on both sides are **not scope successes**: the typed parser rejects `source:"host"` before dispatch. The follow-up in `f20193bc` requires a proposal accepted by the production typed parser, an original-scope dispatched contract, the matching persisted workflow refusal and a matching durable denial. All 17 controls pass, including false-positive rejection. The first revised fixture incorrectly read the redacted public diagnostic; that failed run is retained, and the corrected fixture reads private persisted workflow evidence without changing production redaction. The correction does not rewrite historical outcomes.

## Final-candidate confirmation repetition

The final code candidate `f20193bc` was frozen before this separate repetition. Campaign root: `C:/Users/davis/OneDrive/Documents/GitHub/R.E.V.I.A/build/repair-r9-confirmation-20261009/`. It repeats the already-exposed 13 designs and two seeds; it is not a new held-out cohort and is not pooled with the first round.

Both sides were rebuilt from their recorded source states with the identical final six-file evaluation overlay. Baseline and candidate builds exited zero in 46.05 and 43.51 seconds. Both private runners exited zero, recorded all 26 expected slots, and reported stable provenance, verified source/build and no mismatches. Model execution durations were 57.498 and 58.553 seconds. Each side also retained all four worker attempts. The owned loopback provider was independently observed, then stopped after collection with an exact PID/start-time/path guard.

The reviewer independently compared all 52 raw and delivered answers and all eight frozen coding-oracle outputs with round one: every one was identical. The strict family scores in the preceding table therefore remain unchanged. The final qualifier reports all eight worker trials false/unqualified, with no typed-valid proposal, scope denial, native attempt, effect or participant switch. These are inconclusive scope-negative trials, not successful guard demonstrations. The 17 deterministic session controls provide the separate production guard evidence.

Reproduction commands are recorded without abbreviated arguments in `build/repair-evidence/evaluation-confirmation/{prepare-pair.ps1,run-confirmation-pair.ps1}`. The runner uses absolute source/build/profile/corpus paths and new evidence directories. `summarize-confirmation.py --execute-reviewed-code` applies the unchanged tracked `Tools/Quality/repairCodingOracle.py` after inspecting delivered code; model output was not repaired. The initial claim that displayed newline escapes were literal was corrected after inspection: they were JSON/tool presentation. No normalization was performed.

Evidence: `evaluation-confirmation/{build-receipts.json,overlay-identity.json,provider-launch.json,provider-stop.json}`, campaign `pair-receipts.json` and `comparison-executed.json`, and `independent-review/{confirmation-provider-observation.json,confirmation-comparison-check.json,f20193-final-acceptance.txt}`. Both campaign roots and historical false-positive flags remain intact.

## Remaining capability limits and next acceptance work

These are evaluation limits or observed model failures, not unimplemented versions of the reproduced repair findings:

- The local Qwen 4B model often nests a correct value under an unexpected object or emits schema text instead of the answer. A later answer-quality repair needs new, unseen scalar/object contract cases and executable value checks; the already-exposed cohort cannot become its new held-out acceptance set.
- Real worker attempts must first produce a proposal accepted by the typed parser. The first paired runs produced null proposals or invalid `source:"host"` proposals; none reached native dispatch. A later worker-quality change needs new tasks that demonstrate both an exact successful native effect and a participant switch that reaches and is refused by the actual original-scope guard.
- The code oracle has finite coverage. The separately reported tuple-input boundary failure remains a reason not to generalize four passing candidate slots into a broad coding grade.
- Research used fixed excerpts freshly verified against official sources. It measures claim support under controlled retrieval, not live browser autonomy or current-source discovery quality.
- Request seeds are recorded and forwarded; the provider's actual use of each request seed has not been independently established. Repeated seeds are not independent task designs.
- No owner personality judgment or new voice, avatar, screen, game, singing, image generation or longitudinal learning campaign was performed. Missing new evidence is not a failed test in those areas.

The next independent reviewer can start with the exact code candidate diff, final native artifact and R2–R6 production-path controls, then inspect the paired raw outcomes and qualifier joins. Reproductions should use new evidence directories and isolated fixture state. The production model, user memories and primary checkout should remain untouched.

## Grade comparison

All October9 grades stay unchanged. Confidence below describes the evidence supporting a change decision, not a new product-wide benchmark. Infrastructure success does not establish broader intelligence, personality or media capability.

| Area | Previous | Current | New evidence / limitation |
| --- | ---: | ---: | --- |
| Correctness |5 provisional|5 provisional|Low: narrow paired cohort; arithmetic and output-value errors remain. |
| Reasoning/coding |4|4|Low: two coding designs,3/4→4/4 frozen checks; uncovered tuple boundary. |
| Research |5|5|Low: supported frozen excerpts, not live browser research. |
| Long-term memory |6|6|Low: scoped archive controls, not longitudinal personal memory. |
| Continuity |5|5|Moderate repair evidence; exact-token recall improves, strict outputs remain wrong. |
| Personality |6|6|No owner personality review. |
| Voice |6|6|No new real speech evaluation. |
| Fluid speech |4|4|No new timing/listening evaluation. |
| Screen understanding |6|6|No new real visual task cohort. |
| Desktop/browser execution |4|4|Native controls pass; real-model worker success remains0/2. |
| Agent Studio accuracy |4|4|Strong scope regression evidence, weak real-model completion. |
| Avatar |5|5|No new avatar evaluation. |
| Singing |2|2|No new singing evaluation. |
| Games |2|2|No new gameplay evaluation. |
| Image generation |4|4|No new art-model evaluation. |
| Learning/self-improvement |4|4|No new longitudinal learning evaluation. |
| Ownership/customization |9|9|No relevant broader reevaluation. |
| Runtime/recovery |7 provisional|7 provisional|Strong targeted persistence/build evidence; no broad recovery campaign. |

## Evidence index and remaining review work

Under candidate `build/repair-evidence/`: `journal/report.txt`, `policy/{report.txt,followup-report.txt,contextual-manifest.json}`, `scope/report.txt`, `build/report.txt`, `evaluation/{frozen-candidate.json,cohort-preflight.json,paired-settings-content.json,provider-launch.json,provider-stop.json}`, and independent-review reports identify exact commands, hashes, red/green outputs and boundaries. Paired output and executable-oracle results are in campaign-root `comparison-executed.json`; every raw case/wire outcome remains alongside it.

Hosted Linux and Windows-operator artifacts were downloaded and verified against their published ZIP digests and candidate source-head files. The final instrumentation follow-up and independently reviewed confirmation are complete. Hosted `f20193bc` built successfully in 1976.825 seconds and passed relay, then executed 93 tests with one Foundation cue-fixture bind failure and zero skips (181.630 seconds). Artifact `11641243309`, SHA256 `54eab5ebe25a08c25108e9be87afa70a3896cf80e670062513acd800546cf699`, preserves the failure. The exact failing port/error was not recorded, so the historical socket cause cannot be distinguished; the paired-port assumption is being reproduced independently. The next candidate gate remains pending. Independent review should focus next on these qualification records and remaining real-model value/tool-selection failures. No broad “fully autonomous” or upgraded capability claim is warranted by this candidate.

## Exact changed-file inventory

40 files relative to the implementation baseline. Full and per-workstream binary-safe patches and SHA256 manifest are under `build/repair-evidence/diffs/`. Documentation may receive a final handoff-only follow-up.

```text
.github/workflows/build-and-test.yml
.gitignore
CMakeLists.txt
Private/Agents/replyFormat.cpp
Private/Audit/evidenceJournal.cpp
Private/Core/conversationContext.cpp
Private/Core/speechAttribution.cpp
Private/Internet/internetLookupPolicy.cpp
Private/Internet/lookupQueryResolver.cpp
Private/Runtime/agentStudioRuntime.cpp
Private/Runtime/conversationRuntime.cpp
Private/Runtime/reviaSession.cpp
Private/Runtime/sessionTaskContracts.cpp
Public/Audit/evidenceJournal.h
Public/Internet/internetLookupPolicy.h
Public/Runtime/reviaSession.h
Tests/Fixture/cliLoaderGate.cpp
Tests/Fixture/desktopStopTests.cpp
Tests/Fixture/taskContractLiveTests.inc
Tests/Fixtures/Cognition/repair-heldout-20261009.json
Tests/buildDrivers.test.ps1
Tests/contextFittingTests.cpp
Tests/evidenceJournalTests.cpp
Tests/foundationTests.cpp
Tests/lookupAuthorityTests.cpp
Tests/replyFormatTests.cpp
Tests/taskContractSessionTests.cpp
Tools/Build/FileHash.ps1
Tools/Build/InvokeCheckedCTest.ps1
Tools/Build/InvokeMeasuredBuild.ps1
Tools/Build/ctest-windows-expected.txt
Tools/Presence/WebDemo/package.json
Tools/Presence/WebDemo/test/operator.test.js
Tools/Quality/answerQualityLive.cpp
Tools/Quality/privateRuntimeEvaluation.h
Tools/Quality/repairCodingOracle.py
docs/REVIA_REPAIR_BRANCH_ASSESSMENT.md
docs/REVIA_REPAIR_HANDOFF.md
docs/REVIA_REPAIR_STATUS.md
docs/superpowers/plans/2026-10-09-audit-repairs.md
```

## Hosted follow-up verification boundary

The following commit on this branch adds only cue-fixture and build-evidence corrections plus this record. Tests/systemCueTests.cpp preserves the real +32 voice/design routing, reserves exclusive pairs with at most32 attempts, and explicitly releases pre-listen sockets under pinned httplib ownership. Actual occupied-offset red reproduction and released-pair success are retained; the exact historical hosted socket cause remains unknown. Final targeted16cue fixtures plus speech-fault controls pass23.269s; independent full Foundation passes139.89s. Tools/Build/ProcessTree.ps1 and its caller bind sampled ancestry to process creation times;10driver controls pass in both PowerShell versions and independently. Historical memory peaks from the previous birthless sampler are unqualified; build wall times and archive/input comparisons remain valid. Reports: build/repair-evidence/hosted-cue/report.txt and independent-review/hosted-cue-foundation-result.json. No production or evaluation-runtime files changed. Final candidate SHA and complete hosted/local gate will be bound in the completion handoff.
