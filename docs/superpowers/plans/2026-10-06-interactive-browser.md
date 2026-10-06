# Admitted interactive browser plan

> **For agentic workers:** Use superpowers:executing-plans and test-driven-development for each task.

**Goal:** A bounded stateful browser tool can navigate, observe, fill and click a disposable form, preserve real receipts and refuse stale or unauthorized effects. Existing public read-only research remains unchanged.

**Architecture:** New `Browser` owns an isolated interactive session, worker transport and lifetime; `Actions` dispatches typed requests; Policy admits exact origins and operation grants; Goals retains verification and uncertain-effect handling. Computer consumes observations and proposes typed actions. Root owns CMake, configuration/UI, runtime wiring and the architecture invariant update.

**Scope:** One owned page per session, exact allowed origins, no personal profiles, passwords, uploads, downloads, popups, raw JavaScript, arbitrary selectors or raw CDP exposed to a planner. A working directory or browser profile is not OS isolation. Loopback fixture origins need an explicit separate local-network grant; ordinary public-origin validation remains restrictive.

## Task 1: Typed admission and observation contract

- [x] Add `Public/Browser/browserTypes.h` with separate disabled-by-default settings, exact origin grants, bounded navigation/interact permissions, timeouts, text and element limits.
- [x] Add typed navigate/observe/click/fill requests; element operations require owner session ID, observation generation and opaque element ID. Receipts include URL/title, bounded page text and elements, fingerprint and uncertain-effect flag.
- [x] Native RED-to-GREEN tests cover disabled capability, disallowed origins, private-network refusal, unsupported schemes, malformed fields and scope intersection. Worker tests cover bounded input and stale generations.
- [x] Add parser/policy/action/audit/store/planner mappings without changing research action behavior. Audit raw form values only by size and digest.

## Task 2: Separate project-owned worker

- [x] Create `Tools/Browser/interactiveHost.mjs` and test file; do not modify `browserHost.mjs` or weaken its existing tests.
- [x] Worker owns one new Chromium page and isolated profile under its supplied unique runtime directory. Private inherited protocol pipes replace the proposed network service/token, so there is no listening browser-worker endpoint.
- [x] Host-owned CDP helpers gather DOM affordances and generate opaque IDs in an isolated world; page text remains untrusted. Before fill/click, re-resolve the stored element and verify current document/generation, connectedness, affordance and fingerprint; refuse stale targets.
- [x] Intercept navigations and page requests against exact allowed origins including redirects and private-address checks. Deny downloads, reject dialogs, pause/close additional targets, bypass service workers, block WebSockets and omit password/file controls. This is browser control policy, not a network or OS sandbox.
- [x] Cancellation terminates the owned worker/browser tree. Disconnection after an effect reports uncertainty; never silently retries.
- [x] Node tests begin RED for the absent worker, then pass. Actual disposable local form proves navigation, observation, fill, click, hidden-state result and stale-ID refusal.

## Task 3: Native lifetime and runtime integration

- [x] Add focused native `BrowserSession` child-job owner and bounded private-pipe transport, plus `BrowserExecutor` through ActionRuntime. Capture RuntimeStamp/session; close on authority change, stop, timeout or uncertain effect.
- [x] Recheck admission immediately before each operation and after return. Limit request/response size and operation duration; inherit only explicit worker handles/environment.
- [ ] Root wires conservative machine/goal capability intersection, configuration/defaults/UI, session lifecycle, CMake packaging and diagnostics.
- [ ] A native acceptance fixture runs through ActionRuntime, changes intermediate page state, and verifies the requested result using an original-request criterion. Model proposal or successful click alone cannot establish whole-task completion.

## Evidence and limits

- Record exact commands, RED failures, GREEN suites, browser version and live fixture outcome. Keep deterministic API coverage separate from live model task quality.
- Report unsupported multi-tab, authenticated personal sessions, uploads/downloads and page types honestly.
- Parent acceptance and independent review precede integration. No staging or commits by this worker.

## Recorded execution

- `node --test Tools/Browser/interactiveHost.test.mjs Tools/Browser/interactiveHost.live.test.mjs Tools/Browser/browserHost.test.mjs` passed 7/7. The live test uses installed Edge, a fresh temporary profile and an explicitly admitted disposable loopback form.
- Standalone `browser-contract-check.exe` passed admission/parser/scope tests after the missing helper RED.
- Standalone `browser-owner-check.exe`, launched by `Tools/Browser/interactiveNative.live.test.mjs`, passed actual native worker/job/pipe navigation, field entry, click, hidden-state readback and mid-operation admission revocation.
- `Tests/browserExecutionTests.cpp` exercises the same flow through ActionRuntime, scoped denial, stale target refusal and stop receipt removal. Root owns its shared CMake target and final run.
- A local model has not been evaluated on a held-out browser task corpus. General whole-goal acceptance remains unresolved unless Runtime supplies an original-request criterion; successful tool receipts alone do not assert whole-goal success.

## Bounded exact-content acceptance follow-up

- Live Edge regression first failed because field IDs changed after fill. IDs now remain stable within the isolated document, while receipt generations still invalidate stale operations. Readback returns a full bounded value only for observable editable fields; password/file controls remain excluded.
- `BrowserControlValueIs` checks the original session, URL, newer generation, stable field ID and full exact value. The content gate substitutes the runtime-held original payload only into the unique named field at the explicit original origin; it supplies a read-only browser observation check.
- Final browser acceptance requires fresh Runtime-admitted observation and a prior executed, typed-verified placement. Different origin/session, stale generation, ambiguous field, unavailable or altered value, absent explicit original URL, submission and additional requested work remain unresolved.
- Pure acceptance/typed verification standalone checks passed. Content-gate assertions are included in the existing foundation suite for the final integrated run.
- Native cross-task regression first failed with “Another task inherited the previous task's live browser session.” Matching task ID and policy revision now join the session/grant checks. Different per-action attempt IDs remain allowed. The real native owner harness then passed.
- Fresh Node regression run passed 7/7, including the unchanged research worker checks. Root owns the final full build and integration verification.
