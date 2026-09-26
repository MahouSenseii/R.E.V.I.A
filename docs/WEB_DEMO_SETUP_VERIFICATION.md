# Web demo operator setup verification — 2026-09-25

This follow-up uses R.E.V.I.A base `3e50b4f` and Portfolio base `9e6abf7`.
Changes remain local and uncommitted. No Render service was created, no
production tokens were generated, and Portfolio remains disabled with an empty
relay URL. The public GitHub Pages config was also read and returned disabled.

## Findings and changes

The existing native, relay, bridge and Portfolio API contracts already match.
All six browser API routes exist. Native opts in via REVIA_WEB_ENABLED, binds
127.0.0.1, and requires REVIA_WEB_LOCAL_TOKEN. The bridge defaults to port 17864
and requires WSS in production. No changes were required in native routing,
guest memory isolation, public API routing, or the Portfolio JavaScript.

The deployment/operation gaps were generic CORS examples, a combined example
without a native environment template, no combined Windows starter/status
helper, and no browser test for an actual public relay. This follow-up adds
three separate blank-secret templates, explicit production origin, a Windows
entry point and Node operator, private external configuration instructions, and
an opt-in deployed browser smoke. Real .env files were already ignored by the
WebDemo-specific .gitignore; its exceptions now allow the three new examples.

Two audit corrections: the original attempt to build target `Revia` was an
agent command error; the existing documented target is `ReviaDesktop`. The
root .gitignore is not the only ignore file, and the claim that local environment
files lacked Git protection was incorrect. Both were checked before implementation.

The new Windows helper initially failed because Get-Command returned two Node
executables from PATH. It now explicitly uses the first executable; the actual
PowerShell entry point is covered by a passing subprocess test. No global PATH
or Windows execution policy was changed.

## Observed checks

| Check | Result |
|---|---|
| Exact Render build: npm ci --omit=dev --ignore-scripts | Passed; 0 reported vulnerabilities |
| WebDemo npm test | 46 passed, none failed/skipped; includes 5 operator tests |
| Native npm run test:native | 1 passed against compiled debug guest host; model fixture, not production evidence |
| Full cmake --build --preset debug --parallel 2 | Exit 0; current targets up to date |
| Full ctest --preset debug --output-on-failure --timeout 600 | 12 passed, 0 failed; 101.43 seconds |
| Real local-model browser smoke | Exit 0; Qwen3.5-4B Q4_K_M returned a rendered reply in 1,396 ms |
| Portfolio npm test | 20 passed; data validation passed |
| Portfolio npm run build | Exit 0; existing large homepage video warnings |
| Portfolio browser tests against built output | 36 passed |
| Portfolio browser tests with PORTFOLIO_TEST_SOURCE=1 | 36 passed |
| Windows helper help and check entry point | Passed, including paths with spaces and credential-output checks |
| Secret file ignore check | .env.revia, .env.bridge, .env.relay ignored |
| Opt-in npm run test:deployed | Exit 1 as expected without production configuration; no deployed route verified |

The real-model smoke uses isolated native-host/model processes, actual relay and
bridge, and the built Portfolio, but HTTP/WS loopback transport and a test-only
browser URL rewrite. It uses disposable test credentials; none are production
tokens or persisted in source. It does **not** prove public HTTPS/WSS, the normal
desktop startup lifecycle, or Render ingress behavior. Tests with model fixtures
and browser API fixtures are reported as such, never as real inference evidence.

## Still blocked externally

No actual relay hostname or configured production secrets are available. The
deployed-browser test cannot pass until the operator creates/configures Render,
starts the actual desktop and bridge, and supplies the private invite. It only
overrides the test browser's public configuration; all API traffic must reach
the actual HTTPS relay. It leaves the published configuration unchanged.

Validate Render ingress trust/source limits, deployed cancellation, owner
pause/resume and preemption, separate guests, and disconnect recovery. The safe
default ignores forwarding headers until proxy peers are verified. Do not
enable Portfolio or claim the full production route is verified yet. See
Tools/Presence/WebDemo/WINDOWS.md for exact setup and test commands.
