# Revia repair implementation handoff

The supported repairs, available verification gates and paired evaluation are complete. The final code candidate passes all six hosted CI jobs, including a fresh Windows build, native relay and all 93 required CTest cases; the complete local inventory also passes. Hosted checks also exposed three verification defects: cue-port reservation, PID-reuse contamination in resource sampling, and a relay shutdown assertion made before disconnection. Each has a retained reproduction, a focused correction and independent acceptance. No grade increase is claimed.

## Exact identities

- Audited base: `44aa6bc4d890fe050a4336c530b340d08a3a0767`.
- Implementation and paired-evaluation baseline: `ad207d6f04aad2a6981fa2a26a8f35124f287c0f`.
- Frozen production candidate and first evaluation: **`58c905de45d0146a84c3f5e040e34f049a658380`**.
- Final code candidate: **`fd4f0f1f7d554c9851adcb0680ecd15175e3af87`**. Final local and hosted inventories both pass 93/93 with zero skips or failures; native relay passes on both. Production source remains identical to `58c905de`; later commits correct verification and evidence only.
- Confirmation-evaluation checkpoint: **`f20193bc0a414df81bcb4cf82a0815053baaecb6`**. Six verification files changed; all 659 production file hashes remain identical.
- Candidate branch: `codex/revia-audit-repairs`; worktree: `C:/Users/davis/.codex/worktrees/revia-audit-repairs/R.E.V.I.A`.
- Baseline evaluation worktree: `C:/Users/davis/.codex/worktrees/revia-repair-baseline/R.E.V.I.A`. Production remains at the baseline; the identical six-file evaluation overlay is explicitly captured as dirty source.
- Final GitHub run: [37988019053](https://github.com/MahouSenseii/R.E.V.I.A/actions/runs/37988019053), all six jobs successful on the exact code candidate. Earlier failed and cancelled runs are retained in the verification ledger below.
- Final code review binding: `build/repair-evidence/independent-review/fd4f-review-binding.json`. The first model outcomes remain attributed to `58c905de`; the separately retained repetition is attributed to `f20193bc`. A later handoff-only commit changes documentation, not tested code.

No earlier repair candidate was found in inspected branches, worktrees or implementation records. The available brief was the attached R1–R9 repair prompt (SHA256 `b5c751694e5e3c96aaa66846782ec2a758b6a3c1273ca529d172ffa5bc8ea62e`), the retained independent review, and the explicit R10/R11 implementation request. No separate newer prompt was located. The primary checkout's tracked files, user data, unrelated worktrees and older branch were preserved. Main was not merged as part of this candidate handoff.

## Ownership and review

Root integrated the candidate and owned R6/R8/R9. The policy worker owned R2–R4, the scope worker R5/R11, and the build worker R1/R7/R10. File boundaries and the plan are in `docs/superpowers/plans/2026-10-09-audit-repairs.md`.

A fresh independent agent inspected the actual diff and production callers, reproduced three additional search/continuity findings, and independently reran their final acceptance controls. All three production findings were closed. Final local source and exact JUnit inventory were also independently rechecked in `independent-review/fd4f-local-gate.json`; final hosted and documentation acceptance is recorded in `independent-review/fd4f-final-acceptance.txt`. The reviewer proved all37 hosted/local raw source-hash differences are exactly CRLF conversion; all shared hosted build/test manifest entries match. The reviewer also independently ran the journal, scope, desktop, build-driver, Windows operator and CLI loader checks; inspected raw model outcomes and provenance; and reran the frozen coding oracles. This is an independent agent review, not a claim of an external Claude review.

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

These are the actual final local commands and historical evidence destinations. For a new reproduction, choose new evidence directories/log filenames; do not overwrite these receipts. The complete fresh configuration and compiler arguments are retained in build receipts and the CI workflow.

```powershell
# Run in C:/Users/davis/.codex/worktrees/revia-audit-repairs/R.E.V.I.A
$ErrorActionPreference = 'Stop'
$env:PATH = 'C:/Users/davis/.cmake-deps/cmake/win/x64/bin;C:/Users/davis/.cmake-deps/ninja/win/x64;C:/Users/davis/Qt/Tools/mingw1310_64/bin;C:/Users/davis/Qt/6.8.3/mingw_64/bin;' + $env:PATH
& ./Tools/Build/InvokeMeasuredBuild.ps1 -BuildDirectory build/repair-integrated -Parallel 3 -TimeoutSeconds 300 -EvidenceDirectory build/repair-evidence/build/fd4f0f1f-noop-refresh
if ($LASTEXITCODE -ne 0) { throw 'Frozen all-target refresh failed' }
$env:REVIA_WEB_NATIVE_HOST = (Resolve-Path build/repair-integrated/ReviaWebGuestHost.exe).Path
npm run test:native --prefix Tools/Presence/WebDemo *> build/repair-evidence/build/fd4f0f1f-native-relay.log
if ($LASTEXITCODE -ne 0) { throw 'Frozen native relay failed' }
& ./Tools/Build/InvokeCheckedCTest.ps1 -BuildDirectory build/repair-integrated -EvidenceDirectory build/repair-evidence/build/fd4f0f1f-final-ctest -TimeoutSeconds 600

```

Local fresh configurations additionally set `FETCHCONTENT_SOURCE_DIR_HTTPLIB/JSON/SQLITE/EXPAT` to the pinned dependency source trees beneath the primary checkout's `build/debug/_deps`. No primary build archives were reused. Exact configuration argument arrays, compiler commands, binary hashes and process samples accompany each receipt. Existing evidence directories are immutable records; use new output directories for a reproduction. PowerShell/Git source capture must trust the exact managed worktree; no global trust configuration is required.

### Final verification receipts

Final local result on `fd4f0f1`: all-target no-op refresh passed in 2.5264989s with Ninja reporting no work; native relay1/1 passed with zero failures/cancellations/skips,2235.8629ms. Checked CTest reports 93 expected/discovered/executed, zero failures/skips,320.6283085s; discovery/CTest/qualification all exit0, identityTrusted=true and all five Git capture exits0. Source matches the frozen commit and tracked worktree is clean. JUnit SHA256 `79E9FAB18C4263BECD2DE99504426237CC7952A01C1E9FC24CD6C25DF04032A5`. Receipts: `build/repair-evidence/build/fd4f0f1f-{noop-refresh,final-ctest}`, `fd4f0f1f-native-relay.log`, and `fd4f0f1f-command-lines.ps1`.

Final hosted [run37988019053](https://github.com/MahouSenseii/R.E.V.I.A/actions/runs/37988019053) has all six jobs successful. The fresh native all-target build passed in **1156.6576918s**, exit0, no timeout or source change. Native relay passed **1/1**, zero skips, **2618.9579ms**. Checked CTest reports **93 expected/discovered/executed, zero failures/skips**, **223.3248234s**; discovery/CTest/qualification all exit0, identity trusted and all Git captures exit0. The separate Windows operator suite ran **5/5**, zero skips; Linux journal read/all suites passed, as did the web-text, offline Discord and PowerShell 5.1 jobs.

Native artifact **11645241309** is downloaded and verified: ZIP1355392 bytes, SHA256 `47850908231BDDCE34FAE4B402D0DF99CEA62F956B5AB9EADE8D4B5716393E99`. Hosted JUnit SHA256 `37394A2E4C5ABD3ECB3B9B4949C50CDC1694C288419A5243EFC0135B584E864B`. Both build and test `.ninja_log` files are present at77578 bytes. Extraction: `build/repair-evidence/build/windows-native-fd4f0f1f7d554c9851adcb0680ecd15175e3af87/`; its `ci-evidence/build/` and `ci-evidence/tests/` contain actual build/test receipts, source hashes, commands, configuration and logs. Separate verified artifacts: Windows operator11643528684 and Linux journal11643044528, retained under the matching source-suffixed directories.

The corrected two-second resource sampler observes a peak reachable process tree of11 processes, working1968410624 bytes and private2040270848 bytes. These are sampled instantaneous aggregates for creation-matched build descendants, not total-host memory or a hard process-memory maximum. Short-lived or orphaned descendants can be missed. No OOM claim is made.

The hosted workflow executes these commands after the pinned toolchain/configuration steps; exact environment values and argument arrays are retained with the artifact:

```powershell
$remaining = ([DateTime]::Parse($env:REVIA_CI_DEADLINE_UTC) - [DateTime]::UtcNow).TotalSeconds - 1500
if ($remaining -lt 1) { throw 'No build time remains after reserving relay, CTest and upload time.' }
$budget = [int][Math]::Min(2700, $remaining)
./Tools/Build/InvokeMeasuredBuild.ps1 -BuildDirectory build/ci -Parallel 3 -TimeoutSeconds $budget -EvidenceDirectory build/ci-evidence/build
# In the native relay step: REVIA_WEB_NATIVE_HOST=<github.workspace>/build/ci/ReviaWebGuestHost.exe
npm run test:native --prefix Tools/Presence/WebDemo
# In the CTest step: QT_QPA_PLATFORM=windows
./Tools/Build/InvokeCheckedCTest.ps1 -BuildDirectory build/ci -EvidenceDirectory build/ci-evidence/tests
```

Each workflow step preserves the native exit code. The authoritative complete command sequence is `.github/workflows/build-and-test.yml` at the tested commit; the snippet above omits only setup and log-redirection plumbing.

### Build evidence and retained failure ledger

The repair preserves all required targets and assertions. The same-input archive probe preserves all 231 members and all archive bytes except the symbol-table timestamp:316.622 to 179.369 seconds. Full-symbol versus optimized representative session links were 309.150 versus27.779 seconds; those binaries also include repair changes, so this is not a flags-only comparison. The first integrated all-target build passed in 617.884 seconds. Historical resource samples before the creation-time fix are unqualified because reused PIDs can incorrectly join the sampled process tree; their recorded wall times and artifact sizes remain usable.

Ten build-driver controls cover empty/missing/extra/skipped/failed inventories, untrusted source identity, interrupted build/log retention, successful qualification, hashing when Get-FileHash is unavailable, duplicate CMake PATH resolution and process-birth ancestry. They pass in PowerShell 5.1 and 7 and were independently checked. Exact CTest launch and repeated fixture-directory controls also pass. Historical failures are preserved:

| Checkpoint | Actual result | Follow-up |
| --- | --- | --- |
| Historical audited Windows job113896365749 | Cancelled during a large Debug archive/link build; no CTest result. | Measured bounded build; no evidence supporting an OOM claim. |
| Local `58c905de` |93 executed,92 passed, one BuildDrivers failure; zero skips. | Nested PowerShell hashing and stale fixture-cache handling corrected in `f20193bc`. |
| Hosted run37974101648 / `58c905de` |Cancelled by a later source push at644/684 edges;1648.116s; no relay/CTest success. | Cancellation artifact11638973217 retained; upload-on-cancellation verified. |
| Local `f20193bc` |93/93, zero skips/failures;334.434s; trusted identity. | JUnit `D26B958EFED1D003122FEED33BDB365CF67D6BF81FDBCFDCE62F05BA81E8B7AA`. |
| Hosted run37977328343 / `f20193bc` |Build1976.825s and relay pass;93 executed,92 passed, one Foundation cue-bind failure, zero skips;181.630s. | Artifact11641243309 retained. Exact historical socket port/error was not recorded. Paired-port reservation defect independently reproduced and corrected in `94d5d070`. |
| Local `94d5d070` |93/93, zero skips/failures;318.968s; trusted identity; relay1/1. | JUnit `97CA8697945FC7FF35EA1CFF77B6EC79BE7D8C871700B61F3652FECB3B4BF432`. |
| Hosted run37983534756 / `94d5d070` |All684 build edges passed in1836.156s; relay failed with200 versus expected401; CTest was skipped. | Artifact11644035832 retained. Fixture lifecycle ordering corrected in `fd4f0f1`; explicit CI dependency now permits CTest after a successful build even if relay fails. Relay failure still fails the job. |

The old upload omitted hidden `.ninja_log` files even though drivers copied them locally. `fd4f0f1` enables hidden files only for the existing four owned native evidence paths. Earlier ZIPs remain incomplete for that log; their stdout/stderr, source/config manifests, resource receipts and available JUnit remain retained. The final artifact contains both build and test Ninja logs, verified above.

## Frozen R9 evidence

Campaign root: `C:/Users/davis/OneDrive/Documents/GitHub/R.E.V.I.A/build/repair-r9-20261009/`.

- Candidate: `candidate/`; baseline: `baseline-private-absolute-path/`. Both have runner exit0, 26/26 recorded slots, zero missing slots, stable source/build/provider receipts and no mismatches. The baseline's original `baseline/` directory retains a launcher failure before any model outcome: a relative build path resolved against the process working directory. The absolute-path retry did not selectively repeat failed answers.
- Exact candidate evaluation binary SHA256: `2e97e92e3a4a42f84517f11343666a9e29d68bf691eabe07f1c68b739cbc827d`; baseline: `aac29cc7d4e853cfc8bd347319aa56e1ac6bad689709d37ff1d5cfc7ff8e8cfb`.
- Model: Qwen3.5-4B Q4_K_M, SHA256 `00fe7986ff5f6b463e62455821146049db6f9313603938a70800d1fb69ef11a4`; llama.cpp build10453/commit3cb7ffb1a; 8192 context, one slot, thinking off, loopback18769, RTX5070. Host also has RTX2070 SUPER and128GiB RAM. Authored Revia profile and all 32 configuration file hashes match. Raw settings digests include different absolute worktree paths; relative-path/content equivalence was independently checked.
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

## Confirmation repetition on the evaluated source

Code checkpoint `f20193bc` was frozen before this separate repetition. Campaign root: `C:/Users/davis/OneDrive/Documents/GitHub/R.E.V.I.A/build/repair-r9-confirmation-20261009/`. It repeats the already-exposed 13 designs and two seeds; it is not a new held-out cohort and is not pooled with the first round.

Both sides were rebuilt from their recorded source states with the identical final six-file evaluation overlay. Baseline and candidate builds exited zero in 46.05 and 43.51 seconds. Both private runners exited zero, recorded all 26 expected slots, and reported stable provenance, verified source/build and no mismatches. Campaign runner elapsed durations were 57.498 and 58.553 seconds. Each side also retained all four worker attempts. The owned loopback provider was independently observed, then stopped after collection with an exact PID/start-time/path guard.

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

The final code candidate has no production or protected evaluation-input changes after the evaluated checkpoints. Independent comparison verifies all 659 production files unchanged. R9 evidence is therefore carried forward with that explicit source-equivalence boundary; it is not relabeled as a new run on `fd4f0f1`. Historical false-positive worker flags and failed gates remain available. No broad autonomy or upgraded capability claim follows from this candidate.

The historical `policy/replay.ps1 -Mode Wire` invocation is recorded in the policy report; that helper writes fixed object/executable output paths and must not be rerun in place over the retained evidence. Use the checked CTest inventory with a fresh evidence directory for current regression verification, or copy the helper/output workspace before a focused replay.

Raw local evidence and model outcomes are retained in the named worktrees/campaign roots and are ignored by Git. They are accessible on this PC; cloning the candidate alone does not fetch those raw records. GitHub CI logs/artifacts are available from the final run, subject to repository access and artifact retention. Tracked handoff, status, branch assessment, corpus, executable oracle and tests are in the pushed branch.

## Exact changed-file inventory

43 files relative to the implementation baseline. The full patch is `build/repair-evidence/diffs/fd4f0f1/full-candidate.patch`, SHA256 `539BF96FB5FE3F83E17EA30B3FC05D046D755809BC8A98D69541BE42232A589A`; the exact file inventory is also in `manifest.json`. The earlier scoped workstream patches remain in the parent diff directory. A final documentation-only child commit updates this handoff; tested code remains `fd4f0f1f7d554c9851adcb0680ecd15175e3af87`.

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
Tests/systemCueTests.cpp
Tests/taskContractSessionTests.cpp
Tools/Build/FileHash.ps1
Tools/Build/InvokeCheckedCTest.ps1
Tools/Build/InvokeMeasuredBuild.ps1
Tools/Build/ProcessTree.ps1
Tools/Build/ctest-windows-expected.txt
Tools/Presence/WebDemo/package.json
Tools/Presence/WebDemo/test/native.integration.mjs
Tools/Presence/WebDemo/test/operator.test.js
Tools/Quality/answerQualityLive.cpp
Tools/Quality/privateRuntimeEvaluation.h
Tools/Quality/repairCodingOracle.py
docs/REVIA_REPAIR_BRANCH_ASSESSMENT.md
docs/REVIA_REPAIR_HANDOFF.md
docs/REVIA_REPAIR_STATUS.md
docs/superpowers/plans/2026-10-09-audit-repairs.md
```

## Verification-only follow-ups

`94d5d070` repairs cue reservation without changing production +32 voice/design routing. The fixture exclusively reserves the pair with at most32 attempts and releases pre-listen sockets under pinned httplib ownership. Occupied-offset red, released-pair success, upper-range rejection, exhaustion and cleanup controls are retained. All16 cue fixtures plus speech-fault controls pass; independent Foundation also passes. The historical hosted socket cause cannot be narrowed beyond its recorded bind failure. Creation-aware process sampling separately rejects PID reuse; previously reported birthless memory peaks are unqualified.

`fd4f0f1` changes only the native relay fixture and CI metadata. Existing protocol permits completed-result polling after offline readiness while rejecting new submissions; host disconnection revokes the token. Holding the exact session's native `/end` reproduces the old premature401 assertion as200. The corrected fixture asserts GET200 with expected content and new POST401 while cleanup is held, releases cleanup, observes autonomous host-socket close, and retains the original GET401 assertion. It then re-enables native and checks no connector reconnect or new inference during a bounded regression window. It never calls bridge.stop before the assertions. Focused native1/1, complete relay unit 47/47 and independent native1/1 pass. Production relay, bridge and native authorization are unchanged.

CI now runs checked CTest after successful native build even when the separate relay step fails, while retaining that failure as a job failure. Failed/cancelled builds cannot trigger CTest. Five parsed-workflow controls and independent review pass. Hidden-file upload is restricted to the unchanged four build-evidence paths. Reports: `hosted-cue/report.txt`, `hosted-relay/report.txt`, `independent-review/hosted-relay-review.txt`, and build-driver metadata receipts.
