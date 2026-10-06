# Disposable guest preparation

`Tools/Guest/windows_sandbox.py` prepares an opt-in Windows Sandbox configuration.
It does not install Windows features, reboot, launch a guest, control a desktop,
or run workspace code. This is setup and evidence tooling for delivery package 4;
the Computer/Goals guest adapters and game qualification remain unimplemented.

## Host readiness

Use Python 3.10 or newer, with no additional packages:

```powershell
python Tools/Guest/windows_sandbox.py doctor
```

The JSON is a readiness receipt. Exit 2 means `not_ready`, exit 1 means the probe
could not complete, and exit 0 with `launch_available_unverified` only establishes
that the Windows Sandbox launcher exists. Firmware virtualization, actual guest
startup, GPU behavior, network isolation, and foreground responsiveness still need
live acceptance. The probe never requests elevation or queries optional features
through an elevated process. `featureState` explicitly records that limitation.

On the development host inspected on 2026-10-06, Windows Sandbox's launcher was
absent. The implementation could be tested, but a real guest could not be exercised.
Enabling the feature requires a separate administrator action and may require a
restart. Microsoft documents the virtualization, hardware, and installation
requirements in [Install Windows Sandbox](https://learn.microsoft.com/en-us/windows/security/application-security/application-isolation/windows-sandbox/windows-sandbox-install).

To retain a receipt, pass `doctor --receipt <absolute-new-json-path>`. Its parent
directory must exist. Existing files are never overwritten. Store receipts and
generated runs as runtime data, outside the source tree.

## Prepare a selected workspace

Create a dedicated output parent outside the selected workspace, then pass both
existing directories explicitly:

```powershell
python Tools/Guest/windows_sandbox.py prepare `
    --workspace 'C:\Work\DisposableProject' `
    --artifact-root 'C:\ReviaGuestRuns'
```

Each preparation creates a unique run with these fixed guest mounts:

| Host content | Guest path | Access |
| --- | --- | --- |
| Explicitly selected workspace | `C:\Revia\Input` | Read-only |
| Generated bootstrap and run contract | `C:\Revia\Control` | Read-only |
| Fresh run artifact directory | `C:\Revia\Artifacts` | Read/write |

The configuration disables networking, vGPU, clipboard, microphone, camera and
printer sharing; enables Protected Client; and allocates 4096 MiB. `--network`
explicitly enables networking. `--memory-mb` accepts 2048–16384. Microsoft defines
these XML settings and notes that writable mapped files persist after guest
disposal in [Use and configure Windows Sandbox](https://learn.microsoft.com/en-us/windows/security/application-security/application-isolation/windows-sandbox/windows-sandbox-configure-using-wsb-file).

The tool rejects overlapping mounts, drive/profile roots, system directories,
UNC/device paths, alternate data streams, environment expansion, traversal and
ambiguous Windows names. It checks ancestors and the selected input tree for
symbolic links/reparse points, with a 100,000-entry limit. Select a smaller copied
workspace if these checks refuse the source. A selected folder exposes all of its
contents, including hidden files; use a disposable project containing only the
inputs intended for the guest.

## Launch and evidence

Preparation returns an absolute `launch-manifest.json` path and a sibling
`environment.wsb`. The manifest records the selected mounts, settings, SHA256 of
the configuration/bootstrap/contract, and the current readiness receipt.

```powershell
python Tools/Guest/windows_sandbox.py status 'C:\ReviaGuestRuns\<run-id>\launch-manifest.json'
```

Run `status` immediately before explicitly opening the reviewed `.wsb` in Windows
Sandbox. It rechecks mount boundaries and input reparse points, and refuses
modified control files. The input is not an immutable snapshot; changes after a
check require revalidation. There is no background watcher or automatic launch.

Inside the guest, the fixed bootstrap checks the run contract and expected guest
account, creates the guest-local scratch directory `C:\Revia\Work`, and writes a
new `guest-receipt.json` into the mapped artifacts directory. It executes no
workspace file. Any work should be performed in guest-local scratch space, then
explicitly exported to the artifact directory. Nothing under the host input is
made writable by this tool.

`status` distinguishes `prepared_not_launched`, `invalid_guest_receipt`, and
`guest_reported`. A guest receipt must match the run and report accessible input
and writable artifacts. It is still untrusted data: a host file or guest program
can forge it. `vmIsolationVerified` and `guestControlReady` therefore stay false,
and capture/input/process/reset/reconnect capabilities are all false. Future
adapters must independently establish guest identity, owned lifecycle, admission,
cancellation, observations, and task acceptance before those capabilities change.

Closing Sandbox disposes its guest-local work; the mapped artifacts persist.
Prepare a new run for a clean instance. There is no automatic reset, reconnect,
artifact execution, or destructive cleanup. Existing output is never removed.
Inspect exported files as untrusted artifacts using their actual formats.

## Verification

```powershell
python Tests/guestSandbox.test.py
```

The tests exercise XML and device defaults, explicit networking, disjoint mounts,
Windows path rejection, real junction refusal (Windows), revalidation, fresh run
ownership, manifest drift, and malformed/wrong-run receipts. They do not prove
that Windows Sandbox starts, that the guest enforces the configuration, or that a
Computer/Goals adapter can perform a task. Those remain separate live checks.
