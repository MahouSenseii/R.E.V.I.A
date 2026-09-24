# Public text demo verification record

Implementation began on 2026-09-22 from Revia `5a36472e64554ad186d5522e6cfbe0df624cbfb9` and Portfolio `bfbbc1c8e98af507a2cea064265063b31a5907b9`, both on `main`. Tests below exercise the implementation working tree. The containing commit identifies the exact Revia source; GitHub workflows print `GITHUB_SHA` rather than claiming a self-referential commit hash in this file.

Portfolio was committed and pushed as `9e6abf7c818f8160bd6e59493938f14f27967599`. Its [verification workflow](https://github.com/MahouSenseii/Portfolio/actions/runs/35813905671) and existing [branch-based Pages deployment](https://github.com/MahouSenseii/Portfolio/actions/runs/35813904035) both completed successfully for that SHA. HTTP checks returned 200 for the published page and configuration; configuration was exactly `enabled: false`, `relayUrl: ""`. The [static Revia page](https://mahousenseii.github.io/Portfolio/#revia.html) is published, but there is no deployed live chat relay.

## Changes by repository

Revia adds `Public/Presence/webGuestRuntime.h`, `Private/Presence/webGuestRuntime.cpp`, three `Tests/Fixture/webGuest*.cpp` executables, a stateless public `ConversationRuntime::ReplyPublic` overload/profile, small `ReviaSession` lifecycle hooks, and request cancellation through existing router/model health checks. `CMakeLists.txt` registers the targets/tests. `Tools/Presence/WebDemo` contains relay, outbound connector, strict versioned protocol, tests, lockfile, environment example, Docker/Render recipes and operator instructions. The build workflow adds transport and native integration checks. This document, `WEB_DEMO.md`, and the design/plan record explain the boundary and release procedure.

Portfolio adds `pages/revia.html`, `js/pages/revia.js`, `js/revia/api-client.js`, `css/revia.css`, disabled public configuration, focused Node and browser tests, a test static server, verification/optional Pages workflow and README. Existing router, navigation, project CTA, content schema and package files are updated. An existing invalid 360-degree rotation is normalized to its visually equivalent 0, and compatible vulnerable development dependencies are updated. Existing art, games, music and historical/current Revia links are preserved.

Unrelated pre-existing/concurrent Revia root README and IDE data-source changes are excluded from this feature's commit. No model, dependency directory, build output, owner RuntimeData or test transcript is committed.

## Local commands and results

Environment: Windows, MinGW 13.1.0, Qt 6.8.3, Node 24.15.0, npm 11.12.1, Playwright 1.63.0. Native used a clean `build/web-demo-check` Ninja tree with Debug assertions enabled and `-O0 -g0`; existing downloaded dependency sources were reused. Initial compile/link failures and failing cancellation/capacity regressions were repaired, then rerun. A command is recorded as passing only after normal process exit 0.

| Location / command | Observed result | Exit |
|---|---|---|
| Revia: `cmake --build build/web-demo-check -j 2` | Full build successful after a runtime-file copy retry; native, CLI and Qt desktop targets built | 0 |
| Revia: `ctest --test-dir build/web-demo-check --output-on-failure --timeout 600` | Final source: 10 registered tests passed, 0 failed; 79.24 seconds | 0 |
| WebDemo: `npm test` | 41 passed, 0 failed/cancelled/skipped | 0 |
| WebDemo: `npm audit --omit=dev` | 0 vulnerabilities | 0 |
| WebDemo: `npm run test:native` | 1 complete HTTP/WS/native/model integration passed; 0 failed/skipped | 0 |
| WebDemo: `node test/live.smoke.mjs` | Real Qwen3.5-4B Q4_K_M reply displayed through compiled Portfolio; 1.384 seconds from send to rendered reply; native shutdown exit 0 | 0 |
| DiscordVoice: `npm test` | 18 passed, 0 failed/cancelled/skipped | 0 |
| Portfolio: `npm test` | 20 passed; content schema valid | 0 |
| Portfolio: `npm run build` | Successful; existing homepage video performance warning | 0 |
| Portfolio: `npm run test:browser` | 36 passed against compiled output | 0 |
| Portfolio: `PORTFOLIO_TEST_SOURCE=1` then browser command | 36 passed against original ES modules used by branch Pages | 0 |
| Portfolio: `npm audit` | 0 vulnerabilities | 0 |

The CTest registration includes guest lifecycle, private-source canaries, foundation, identity final save, operator session, browser policy, Discord voice, Qwen policy, SVG renderer and desktop smoke. Opt-in real external-service suites are not represented as exercised merely because offline CTest passed. No skipped or unregistered suite is counted as a pass.

The initial full native build and earlier recheck also exited 0. A later `-j 8` rebuild compiled/linked code but exited 1 while the existing post-build command copied `Tools` into the runtime directory. Retrying the outstanding build at `-j 2` with the source files stable completed normally; no assertion or build command was removed. Nonfatal Qt deployment warnings about optional DX compiler/Vulkan support remain environmental limitations.

## What the tests demonstrate

The native privacy test populates real owner profile/history/compressed history, mood cause, preferences, relationship and screen-context providers; real archive/capture/internet callbacks; and a real temporary durable memory database. Positive-control owner requests prove these sources can enter actual serialized model requests. SQLite connection/statement instrumentation first observes a real owner memory read, then observes zero guest database access. Guest turns produce zero owner provider/executor/event callbacks, zero extra embedding requests, unchanged owner state and unchanged temporary filesystem contents. Guest A's synthetic marker never enters guest B's actual model request or delivered response. Curated Revia character remains present, and tool/function schemas are absent.

Current-interest and unresolved-thought markers are exercised through the actual state-packet renderer and owner router request, because the production controller does not expose setters for arbitrary values in those fields. Cached camera/clipboard markers use the real screen-context provider; no physical camera/clipboard is accessed. The guest component does not accept shell, file or permission executors; tests do not claim counters for capabilities that do not exist in its interface. This is source and execution evidence for the application boundary, not an OS sandbox or proof against arbitrary malicious local software/model infrastructure.

Native lifecycle tests verify strict authentication/body/identity handling, final filtering, bounded separate histories, cancellation before acceptance, during model discovery and during generation, owner busy rejection/preemption, replay rejection, end/pause suppression and repeated visitor turnover beyond the ten-live-session cap. The identifier tombstones preserve rejection without retaining ended histories. The relay/connector suite verifies real HTTP/WS serialization, exact origins, admission, bounded queues/rates/retention, epoch isolation, retries, cross-session ownership, cancellation unwind, restart/loss and authoritative owner-off behavior.

The native integration harness sends HTTP requests through a real relay and WS connector to the compiled native guest runtime and a recording model HTTP server. It verifies two guest identities, the actual final response, one inference for an idempotent retry, owner preemption, cancelled-history suppression, sequential guest cleanup and old-token invalidation. Browser tests separately exercise literal text rendering, keyboard/IME, route cleanup, late asynchronous results, availability, retries and both root/project base paths at desktop/mobile sizes. Screenshots were visually inspected.

The real smoke used the existing bundled llama.cpp server and existing Qwen3.5-4B-Q4_K_M model in owned loopback child processes with reasoning disabled for this short test. The browser submitted a harmless maple-leaf question and displayed: “A maple leaf is fascinating because it's a living masterpiece that turns from vibrant green to fiery red, signaling the start of nature's grand autumn dance!” This verifies actual inference and final delivery, not factual accuracy of arbitrary model answers. Session end, relay observation of connector shutdown, native exit 0 and smoke process exit 0 all completed. Earlier harness runs failed because a button selector used the wrong accessible name and because an immediate shutdown assertion raced the socket-close event; both test defects were corrected without weakening the required final states.

## Reproduce the opt-in real-model smoke

Build Revia's `ReviaWebGuestHost` and Portfolio's `dist`, install Portfolio's Chromium test browser, then set local paths in your shell. These are paths, not credentials:

```powershell
$env:REVIA_WEB_PORTFOLIO_ROOT = (Resolve-Path ..\Portfolio).Path
$env:REVIA_WEB_REAL_MODEL = (Resolve-Path Models\Qwen3.5-4B-Q4_K_M.gguf).Path
Set-Location Tools\Presence\WebDemo
node test/live.smoke.mjs
```

Optional `REVIA_WEB_NATIVE_HOST` and `REVIA_WEB_LLAMA_SERVER` override the default test executable and bundled model server. The smoke owns isolated child processes and a temporary runtime root, generates disposable in-memory credentials, and stops only its own processes. It exercises the compiled Portfolio page, relay, outbound connector, native filtered public path and existing real local model. A test-only browser URL rewrite forwards actual requests to loopback because production frontend configuration correctly refuses localhost/HTTP. It does not mock model replies or prove public TLS/DNS, hosted proxy behavior or production desktop lifecycle wiring.

## Deployment gates and limitations

No Render service/account or paid plan was created. Public relay TLS/WSS, real ingress trust/source buckets, sleep/recovery, hosted quotas and a deployed two-guest smoke remain owner deployment steps. Docker packaging was supplied but the Docker executable was unavailable locally, so no image-build/run claim is made. Keep Portfolio disabled until these checks pass. Configure the independent host bearer, native local bearer and invite (only hashes on relay), exact browser origin, WSS URL and verified proxy peers using [the transport guide](../Tools/Presence/WebDemo/README.md). With no trusted proxy range, forwarding headers are ignored and hosted visitors can share a conservative socket-peer rate bucket.

The relay can read guest text; infrastructure/operator retention is separate from bounded application memory. Public mode grants no desktop authority and stores no durable owner memory. Process restarts revoke sessions and reset the process-local daily budget. Native model configuration changes require a runtime/connector restart. Existing non-web Fast/Expert/context-fallback health checks can still wait through their health timeout when cancelled; the isolated guest router cannot enter those paths. Voice remains a future separately scoped phase.
