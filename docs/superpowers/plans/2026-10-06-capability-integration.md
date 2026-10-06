# Capability upgrade integration plan

> For agentic workers: execute the approved design through bounded domain tasks and independent review. The owner selected execution with "Do these" and retains standing integration authorization.

**Goal:** Connect reliable continuity, command/computer operation and real art to Revia's existing runtime, then extend independently qualified operation.

**Architecture:** Existing owners retain model routing, companion identity, memory, policy, execution, resources and presentation. Native process execution gains one focused owner. Revia's parent identity remains the speaker across specialist models.

**Tech stack:** C++20, Qt 6.8, SQLite, llama.cpp, Python/Diffusers, CMake/Ninja, Windows.

**Spec:** `docs/CAPABILITY_UPGRADE_DESIGN.md`.

## Constraints and review focus

- Preserve unrelated primary-checkout IDE/CMake changes and all private runtime data.
- Use the isolated capability-upgrades worktree; integrate only verified task changes into main and push under the owner's standing instruction.
- Keep authored personality intact; semantic correctness and personality are evaluated separately.
- Scope follows captured companion, audience and participant through queued work.
- Model completion is a proposal; native results and task-specific acceptance establish success.
- New process/browser capabilities have explicit contracts; a working directory is not process isolation.
- Tests cover late corrections, scope changes during work, unavailable model/runtime, cancellation, uncertain effects and restart without duplicate actions.

## Owned packages

| Package | Owner | Produced interfaces / integration | Current status |
|---|---|---|---|
| Continuity | Level 2 continuity supervisor | Existing context/prompt owners; `conversationContext::RestoreMessages(source, recentMessages)`; exact backend token fitting with conservative fallback; actual delivered tier. | Implementation and focused checks complete. |
| Process and operator | Level 2 operator supervisor | `ProcessRequest/Result/Settings`; typed `execute_process` and guarded `write_text_file`; GoalRunner recovery and independent completion evidence callbacks. | Implementation and final combined regression complete. |
| Art | Level 2 art supervisor | `DrawingRequestPolicy::Classify`, cancellable `ImageGenerator::Generate`, `Cancel/Snapshot/Unload`, admitted `DocumentWorkshop::GenerateImage`. | Implementation and two-provider live acceptance complete within the limits below. |
| Runtime/config/build | Level 3 director | Session routing/admission/cancellation, settings persistence and controls, CMake registration, resource setup and final acceptance. | Verified and delivered to main. |
| Scoped memory/archive | Continuity supervisor and Director | Person attribution, conservative legacy-unattributed migration, scoped recall/restore, guarded introductions and admitted learning/reflection. | Implementation, focused storage/relationship checks and independent review complete. |
| Interactive browser and tool workers | Operator and continuity supervisors | Owned browser session/receipts; bounded tool workers through existing authority, cumulative budgets, reviewer and parent acceptance. | Implementation and final integrated native fixtures complete. |
| Guest preparation and measurements | Independent review/support agent | Opt-in Windows Sandbox doctor/configuration/receipt tooling; fixed direct-backend 16K/32K measurements. | Tooling and measurements complete. Live guest adapters and game qualification remain outstanding. |

## Integration steps

- [x] Build the original production foundation with Qt-matched MinGW 13.1 before product edits; record deliberate new RED cases separately from historical failures.
- [x] Register focused existing/new suites: context fitting, process execution/fixture, operator recovery, drawing intent, artifact verification and settings validation.
- [x] Wire `Classify` into the natural drawing branch; pass turn cancellation and captured admission to image generation and check before publication.
- [x] Add process/browser settings load, validation, persistence and compact controls through existing capability editor/session paths.
- [x] Connect operator recovery to supplied observations and final acceptance to independent runtime evidence; carry structured process output through the existing goal observation path.
- [x] Reconstruct scoped restart continuity from the bounded archive through `RestoreMessages`; preserve recent exchanges and source-linked older continuity.
- [x] Install the owned image runtime and run real image creation, active cancellation, restart and unload acceptance with SD-Turbo and SDXL-Turbo, recording GPU use separately from visual quality.
- [x] Complete scoped memory/archive migration, owned interactive browser and bounded tool-worker implementation, with dedicated regressions and explicit uncertain-effect recovery.
- [x] Deliver guest preparation/readiness tooling and tests; accurately report the host's missing Windows Sandbox launcher.
- [x] Run three fixed direct-backend probes each at 16K and 32K, retain exact traffic and measurements, and leave context defaults unchanged.
- [x] Complete independent source review and address material findings, including operator registration revision, browser task ownership, tool recovery/token charging and art audit acceptance.
- [x] Complete the Director's final combined native/Qt build and relevant regression run against the integrated source, including the latest operator and worker fixtures.
- [x] Integrate tested commits into main, push, verify remote head and clean up the task's merged branch/worktree.
- [ ] Separately deliver and exercise real guest capture/input/process/reset/reconnect adapters after host readiness; qualify a game-specific policy before making game competence claims.

## Current execution ledger

2026-10-06: created managed worktree at `C:/Users/davis/.codex/worktrees/capability-upgrades/R.E.V.I.A`, branch `codex/capability-upgrades`, baseline b2fc080. Three real domain supervisors initially worked in parallel and completed their bounded implementations. Follow-up browser, scoped storage and tool-worker work reused actual agents; no subordinate executions are inferred from role labels. Director owns integration. Per-agent model token and timing telemetry is unavailable.

| Real hierarchy | Status at this ledger update | Remaining responsibility |
|---|---|---|
| Level 3 Director (`/root`) | Verified, delivered and cleaned up | No remaining work in this delivery; guest adapters and game qualification remain separately outstanding. |
| Level 2 continuity supervisor (`/root/continuity_implementation`) | Finished; product files frozen | Respond only to concrete integration failures. Scoped storage, relationship and existing workflow suites plus new parser/budget/recovery checks passed in focused runs. |
| Level 2 operator supervisor (`/root/operator_implementation`) | Finished; product files frozen | Final fixtures passed the Director's combined regression. |
| Level 2 art supervisor | Finished | Native live-provider results and quality limitations handed off; no active art implementation remains. |
| Independent review/support (`/root/integration_review`) | Review and support complete; ledger handoff | Final bounded review found no remaining P1/P2 after rereading fixes. Guest/context/proxy checks passed; no additional product edits or shared builds. |

Preflight: continuity and art both need ReviaSession hooks; only Director edits that file. Operator and Director share public action types through agreed Process interfaces; operator edits action contracts, Director edits permission serialization/UI. All targets share one Ninja directory; only Director runs CMake/Ninja there. Art Python/standalone checks and worker standalone C++ REDs are independent. No plan requires overriding personality or treating model claims as verification.

Ruling: user execution authorization covers the reviewed design and routine reversible implementation decisions; no repeated plan-approval pause is required. Baseline production library compiled before workers began product edits; new focused tests are explicitly RED evidence, not historical failures.

## Recorded evidence and limits

**Art:** Native Windows acceptance generated and independently decoded/hashed real 512×512 images on the RTX 5070 with four steps and seed 42. SD-Turbo took 11.68 seconds per native request, with 3096/3714 MiB peak PyTorch allocated/reserved memory and 94.5 ms active-generation stop latency. SDXL-Turbo fp16 took 17.01 seconds, with 7836/9030 MiB and 81.2 ms stop latency. Both restarted after cancellation, released the worker, and reproduced the same image bytes for the fixed seed. Neither sample satisfied every prompt detail; cross-scene character consistency, reference editing, ComfyUI workflows and concurrent chat/speech throughput remain unqualified. See [image evidence](../../IMAGE_GENERATION.md).

**Context:** The installed Qwen3.5-4B Q4_K_M backend completed three fixed probes per context at approximately 75% occupancy. Exact preflight counts matched reported usage for all six replies, with zero cached prompt tokens. Reply latency was 3.29–3.60 seconds at 16K and 6.01–6.24 seconds at 32K. RTX 5070 system-wide sampled peaks were 5632 and 6221 MiB against a 2202 MiB baseline; these are not exact model allocations. Strict acceptance was **0/3 at each size** because of source-ID formatting and an unexpected JSON wrapper, despite manual inspection finding the requested non-source facts correct. This direct-backend run does not measure Revia's full continuity pipeline, personality, concurrency or time to first token. Defaults remain unchanged. Both owned servers were stopped. See [context evidence](../../CONTEXT_BENCHMARK.md).

**Guest:** The opt-in doctor/generator validates canonical disjoint mounts, read-only selected input, separate writable artifacts, XML escaping, reparse-point refusal and immutable control-file hashes. Clipboard/networking defaults are disabled. Thirteen Python checks passed and the fixed PowerShell bootstrap parsed successfully. The host doctor reports `not_ready`: Windows Sandbox's launcher is absent and feature state cannot be established through an elevated query in this session. No feature was enabled, guest launched or reboot attempted. Guest self-reports remain untrusted; VM isolation/control and capture/input/process/reset/reconnect stay unverified or unavailable. A mounted folder is not VM isolation. See [guest readiness](../../GUEST_ENVIRONMENT.md).

**Review and focused verification:** Final rereads confirmed guarded relationship mutation outside audience/speaker locks, participant-scoped reflections and organizer inventory, provider destination checks under the image lock, cancellable image publication, post-registration operator revision capture, browser task ownership, audit-aware art acceptance, persisted per-call token charging and refusal to replay interrupted tool effects without fresh recovery evidence. Guest checks (13), context harness checks (5), the standalone real-loopback accounting proxy fixture, and focused worker/parser/budget/existing-workflow suites passed. These results do not replace the pending final combined regression. Native process execution still uses host permissions rather than VM isolation; final operator acceptance currently supports specific independently observable content criteria, not arbitrary model claims of completion.

**Final integration verification:** The Qt-matched MinGW build completed successfully. All 44 selected CTest suites passed: 38 component suites, companion isolation/migration, and five final foundation/operator/identity/terminal/desktop suites. The full foundation suite passed in 141.47 seconds. Seven separate Node browser checks also passed. Real Qt captures at 760×540, 1040×720 and 1600×1000 cover the permission cards and Agent Studio. Integration fixes retained a promoted speaker context through the complete turn and preserved supervised mode through saved worker scope; regressions also check audience revocation, unattended risk limits and actual restored-policy process execution. Older positive-recall fixtures now supply explicit participant scope, and the WAL migration fixture names its columns after the schema extension.

**Packaged application:** `build/capability-release/ReviaDesktop.exe` contains the completed application build and its Qt/compiler dependencies. A read-only import check found no unresolved imports across 23 binaries and 195 import references on this host. Its SHA256 is `52FD12597DFFE90501D531C881956EEDADA6C25CE538AD596D9E86D0FF845AF9`. The source-build desktop smoke test passed. Automatic approval review blocked the separate packaged-app smoke command with "blocked by policy" and no specific reason, so standalone package launch is not claimed as verified. This local package resolves the primary checkout's Config and Tools rather than establishing another runtime root.

Preserved local live evidence is under `C:/Users/davis/OneDrive/Documents/GitHub/R.E.V.I.A/build/capability-evidence-20261006/`, including `art-acceptance-sd-metrics`, `art-acceptance-sdxl`, `context-benchmark-20261006`, `ui-renders` and `verification`. Runtime reports and generated images remain user data rather than tracked source.

**Git delivery:** Implementation commit `608e2eb74a413bb60495271fb462872d02863625` was fast-forwarded into main and pushed to GitHub; the remote main SHA was verified. Four focused native checks passed again from the merged primary checkout. The managed capability-upgrades worktree was archived after preserving its live evidence and remaining runtime data, and the merged local `codex/capability-upgrades` branch was deleted. The owner's pre-existing local IDE/CMake changes and `.output.txt` remain uncommitted and preserved. Real guest adapters and game qualification remain outside this completed delivery.
