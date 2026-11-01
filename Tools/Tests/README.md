# Existing test catalog and launcher

`ReviaTestLauncher` and typed `/tests` share one owner over existing CTest registrations. They discover, select and report independent modules; they do not build dependencies or start model, speech or GPU benchmarks. Use the standalone CLI for developer checks without starting Revia's application session.

## Prepare an existing developer build

Use a configured build of this source tree with `BUILD_TESTING=ON`, its existing C++20 toolchain/dependencies, and a native CTest executable supporting JSON discovery and JUnit output. Desktop `/tests` also requires the normal application build. Packaged releases may omit developer test binaries.

Build the launcher and the current safe defaults through the existing CMake targets:

```text
cmake --build <absolute-build> --target ReviaTestLauncher ReviaVideoPublicationLaneTests ReviaDrawingIntentTests ReviaVideoRequestPolicyTests ReviaVideoFrameReplyTests ReviaResponseLatencyTests
```

For a multi-config generator, also supply `--config <configuration>` and use that same configuration below. This command compiles targets; it does not run them. Add targets required by another reviewed group only after checking its prerequisites.

The checked policy is [`Config/Tests/quick-allowlist.json`](../../Config/Tests/quick-allowlist.json). `quick` currently contains four pure checks: publication-lane ownership, drawing intent, video-request policy and sampled-frame reply parsing. `response` contains synthetic timestamp/sample-window logic. It measures no real response, playback or GPU latency. Approved groups are policy data: inspect the catalog rather than assuming only these two exist. Unknown, full, live and hardware checks remain held until their prerequisites and execution policy are reviewed.

Other approved groups may use controlled fixtures with fresh staged CTest profiles and environments. Read each module's catalog prerequisites before selecting it; mocks and Qt panels have different requirements. A fixture's loopback connection may be blocked by the execution environment. The launcher does not install dependencies, request broader permissions or change security settings to make it run. Broader reviewed-group selections are explicit and include only their approved members, not the entire discovered catalog.

## CLI: discover, select and resume

PowerShell examples below use placeholders; replace them with absolute local paths. The build's `CMakeCache.txt` must identify `$sourceRoot`. Choose an owned result root that does not overlap the source tree or equal the build root, and a different result root for each concurrent launcher instance.

```powershell
$sourceRoot = '<absolute-source>'
$buildRoot = '<absolute-build>'
$ctest = '<absolute-ctest.exe>'
$outRoot = '<absolute-separate-results-root>'
$launcher = '<absolute-ReviaTestLauncher.exe>'
$devArgs = @('--source-root', $sourceRoot, '--build-root', $buildRoot,
    '--ctest', $ctest, '--source-id', '<declared-revision-or-tree-id>',
    '--output', $outRoot, '--policy', "$sourceRoot/Config/Tests/quick-allowlist.json")
# Multi-config only: $devArgs += @('--configuration', '<configuration>')
# If DLL/tool dependencies require it, supply an explicit minimal path:
# $devArgs += @('--runtime-path', '<dependency-directory;another-directory>')

& $launcher list @devArgs --json
& $launcher run @devArgs --group quick
```

`list --json` includes every independent registration, purpose, group memberships, prerequisites, availability and latest evidence applicability. `run` defaults to `quick` when `--group` is omitted. Select another approved group with `--group <group>`; a group ID alone grants no execution permission. The launcher revalidates source/build/catalog/config identity before execution.

The launcher prints `Results: <run-directory>/results.json`. Set `$run` to that file's **containing directory**, not the JSON file:

```powershell
$run = '<absolute-prior-run-directory-under-outRoot>'
& $launcher status --output $outRoot --from $run
& $launcher result --output $outRoot --from $run
& $launcher resume @devArgs --from $run --group '<approved-group>'
```

`status` reads a saved checkpoint; it does not establish whether a process is still alive. `result` prints its complete JSON. Resume creates new output and reuses only eligible observations whose captured identity still matches. Prior runs remain unchanged. Press Ctrl+C to request cancellation; the owner joins its worker on shutdown. Exit codes: `0` selected rows passed; `1` completed with another outcome, including reuse; `2` setup/run failure; `3` cancelled.

## Typed `/tests`

Set these variables in the environment that starts the developer application, then restart the session after changing them. No command text can configure paths or replace the policy.

| Variable | Value |
| --- | --- |
| `REVIA_TESTS_ENABLED` | `1`; otherwise tests are disabled |
| `REVIA_TESTS_SOURCE_ROOT` | Absolute source root |
| `REVIA_TESTS_BUILD_ROOT` | Absolute configured build root |
| `REVIA_TESTS_CTEST` | Absolute native CTest executable |
| `REVIA_TESTS_POLICY` | Absolute policy file inside the source root |
| `REVIA_TESTS_OUTPUT_ROOT` | Separate owned result root |
| `REVIA_TESTS_SOURCE_IDENTITY` | Declared revision/tree identity; computed fingerprints are also recorded |
| `REVIA_TESTS_CONFIGURATION` | Optional actual multi-config build configuration |
| `REVIA_TESTS_RUNTIME_PATH` | Optional explicit minimal dependency PATH; parent PATH is not inherited |

- `/tests`: refresh metadata when idle, including after failure or cancellation, and show the compact group menu. It never starts tests and leaves active work alone.
- `/tests catalog [page]`: inspect all modules in 16-row pages, including held registrations, prerequisites and applicability.
- `/tests run <approved-group>`: start the selected policy group asynchronously. `/tests quick` and `/tests response` are conveniences. IDs accept only ASCII letters, digits, `_` and `-`, up to 64 characters; paths, regex and shell text are refused.
- `/tests status`: read cached progress and outcome/fresh/reused counts. `/tests details` or `/tests last`: inspect recorded outcomes and evidence paths.
- `/tests cancel`: request cancellation; check status for its actual outcome. Session shutdown closes admission and joins owned work. Voice and model proposals cannot launch developer tests.

## Read blocked states and evidence honestly

Disabled configuration, missing CTest/build binaries, a build cache for another source tree, an invalid policy/output path, or an unreviewed registration remain blocked or held. Nothing is installed or built automatically. On discovery failure, inspect the diagnostic and retry `list` or bare `/tests` after the owner is idle. If captured inputs changed, rebuild the affected targets as needed and rediscover before a new run or resume.

Each fresh run writes `results.json`, `results.junit.xml`, bounded per-module logs and CTest JUnit receipts. Keep the run directory intact: identities and artifact digests govern reuse. Identity covers captured source/build/configuration and selected execution inputs; its applicability remains partial. A declared revision proves neither rebuild freshness nor complete external dependency coverage. `reused` is historical and `fresh=false`; skipped, blocked, unverified, cancelled, timed-out and not-run modules are not passes. Interrupted or stale observations retain raw evidence without becoming fresh passes. Manual context/TTS/GPU tools remain separate workloads requiring their own explicit setup and observed measurements.
