# Automatic Ubuntu workspace

Revia can prepare and start an enrolled Ubuntu 24.04 VM through VMware Workstation.
The Windows session owns conversation, personality, memory, approvals, task scope,
and audit. The guest runs a typed worker; it contains no second companion or model.

This currently provides headless coding tools. Guest desktop, browser automation,
window lookup, and Windows application execution are not implemented by this bridge.
Windows provider qualification has exercised provisioning, first-boot readiness,
survival after the startup process job closes, typed write/execute/readback, all 14
Linux worker regressions, and soft shutdown. Initial qualification exposed a
VMware address-discovery dependency and used a diagnostic query; the bootstrap now
issues that bounded waiting query itself. A subsequent unattended restart,
readback, and shutdown passed in the selected CLion runtime root without external
diagnostic queries. Interactive planning and desktop UI qualification are separate
from this provider proof. The existing iterative operator still applies desktop
content extraction and whole-task acceptance rules: general coding requests through
`/operate` are not qualified as complete by this bridge. Verified guest steps alone
must not be described as verified completion of an arbitrary coding task.

## Host prerequisites

- Windows with working firmware virtualization and a usable VMware Workstation
  installation. The bridge expects `vmrun.exe`, `vmcli.exe`,
  `vmware-vdiskmanager.exe`, and `mkisofs.exe` under
  `%ProgramFiles%\VMware\VMware Workstation`.
- Windows OpenSSH client tools `ssh.exe` and `ssh-keygen.exe` under
  `%SystemRoot%\System32\OpenSSH`.
- A real Python interpreter configured by `speech.pythonExecutable`; startup uses
  its resolved absolute path. The bridge uses the standard library and runs Python
  with `-I -B`. Python 3.12 is the exercised host version. The default configuration
  points to `ThirdParty/QwenTTS/.venv/Scripts/python.exe`.
- Network access to Canonical's cloud-image server and Ubuntu package repositories.
  The bootstrap downloads the pinned [Ubuntu image release](https://cloud-images.ubuntu.com/releases/noble/release-20260926/)
  over HTTPS and checks its fixed SHA256 before use. It does not download a Windows
  ISO or infer any Windows license entitlement.
- At least 12 GiB free disk for provisioning. A stopped VM starts only with at least
  3 GiB available host RAM and 6 GiB free disk.

Admission uses the current machine's available RAM, not a fixed laptop capacity.
The same bridge can run on a larger Windows PC with the prerequisites installed;
that PC has not been physically qualified by this task. The guest currently keeps
the conservative 2-CPU/2-GiB allocation even on larger hosts. It does not resize an
existing enrollment or change Revia's model, voice or personality automatically.
Full neural voice remains enabled. When resources are insufficient, conversation
stays available and the VM reports a resource block; relaunch retries admission.

`Tools/Setup-Revia.ps1` does not install VMware or OpenSSH, configure firmware,
grant administrator rights, or enable this workspace. Missing host tools stop setup
with a diagnostic; the bootstrap does not launch an installer or change host
virtualization/security settings. Guest packages are installed automatically using
a fixed [cloud-init NoCloud seed](https://docs.cloud-init.io/en/latest/reference/datasources/nocloud.html).
The seed explicitly installs OpenSSH server and Open VMware Tools, then starts
the VMware Tools service before readiness; guest-address discovery depends on it.

## Launch root and configuration

From a development checkout, use:

```powershell
.\Tools\RunDesktop.ps1
# Or: .\Tools\RunDesktop.ps1 -Release
# Explicitly use an existing CLion profile while running the rebuilt executable:
.\Tools\RunDesktop.ps1 -RuntimeRoot (Resolve-Path .\cmake-build-debug).Path
```

The launcher selects the build executable and explicitly passes the absolute
runtime root through `--runtime-root`; its default is the repository. `-RuntimeRoot`
selects an existing profile without copying its stores. The desktop requires that
root to contain `Config`. This binds private stores to the intended runtime root; launching another
copy must not search for, copy, or silently adopt a different private VM enrollment.

The provider holds an operating-system lease for its enrollment from preparation
through provider destruction. Another Revia process is refused before VM setup,
actions or shutdown; an owner process exiting releases the lease. This protects
the VM, not every shared profile store, so still run one Revia process per profile.
The maintenance CLI is a separate entry point: run commands that change guest
state only while that profile's Revia session is stopped.

There are two independent settings:

1. **Companion startup:** `RuntimeData/VirtualMachine/settings.json`, resolved
   through the selected companion's private paths. For the original `legacy`
   companion in the selected runtime root, its contents are:

   ```json
   { "schemaVersion": 1, "enabled": true, "ownerId": "legacy" }
   ```

   Other companions must use their actual opaque owner ID. A missing file disables
   startup. Malformed, oversized, redirected, duplicate-key, or mismatched settings
   cannot enable it. The VM is stored beside this file in `Guest`.

2. **Action permissions:** the `guest` object in the runtime root's
   `RuntimeData/Capabilities/capabilities.json`. Checked-in
   `Config/capabilities.json` supplies initial defaults, all disabled. `enabled`,
   `read`, `write`, `execute`, `allowTaskExecution`, approved executables, and limits
   govern actions independently of startup. Starting a VM grants no action authority.

Startup is asynchronous and leaves conversation available. Its existing worker
waits up to 180 seconds for successful neural-voice preparation before admission;
failed or unfinished preparation blocks the VM without changing voice settings.
If no voice warmup is needed, admission proceeds immediately. The helper then
checks current memory and disk before powering on a stopped guest. It creates or reuses
the owner-bound enrollment, verified image cache, SSH keys, sparse disk, and VM
configuration, then requests `nogui` startup. It never adopts an existing directory
without its matching manifest. SSH keys have a user-only host ACL; the guest host
key is pinned, forwarding is disabled, and a root-owned forced command invokes the
isolated Python worker. There are no host filesystem mounts, shared folders,
clipboard sharing, or drag-and-drop integration. Networking uses VMware NAT.

## Typed operations and limits

`vm_list_directory`, `vm_read_text_file`, `vm_write_text_file`, and
`vm_execute_process` are distinct from host actions (the chat code tools add
`vm_run_python`, `vm_search_files` and `vm_share_project`; see below). File paths are relative POSIX
paths within `/home/revia/workspace`; the host does not reinterpret them as Windows
paths. Requests and receipts bind to the enrolled environment and current boot ID.
A guest failure never selects a host executor as fallback.
Task actions retain their admitted conversation scope. Changing audience or
participant can stop them; a later conversation cannot replace that scope or its
cancellation ancestry. Ordinary conversation within the same scope remains available.

The fixed helper executable set is `/usr/bin/python3`, `cmake`, `ctest`, `ninja`,
`git`, `node`, `npm`, `g++`, and `gcc`, each with the `/usr/bin/` prefix. Host policy
can narrow this set. Process arguments are passed as an array without a shell.
These interpreters can run arbitrary guest code: the VM is the isolation boundary,
not the process working directory or the executable allowlist.

| Resource | Default / ceiling |
| --- | --- |
| VM | 2 virtual CPUs, 2048 MiB RAM, 32 GiB sparse disk |
| Text read/write | 65,536 bytes each; ceiling 262,144 each |
| Combined process output | 65,536 bytes maximum |
| Directory entries | 256; ceiling 512 |
| Process deadline | 30 seconds by default; ceiling 120 seconds |
| Relative path | 2,048 UTF-8 bytes; 255 per component |
| Arguments | 128; 8,192 UTF-8 bytes each; 65,536 combined with executable/cwd |

Writes require `expectedDigest: "missing"` for creation or the existing file's
lowercase SHA256. Failures before publication preserve the original; a failure
after publication can leave an uncertain effect. Helper requests are serialized,
but an independently running guest process can race the final digest check and replacement; this is not kernel-atomic
compare-and-swap.

## Code interpreter and coding tools

The chat tools `run_python`, `share_project`, `repo_grep`, `read_file_range`,
`apply_patch`, `run_tests` and `export_patch` run in this guest, through the
same typed guest actions, capability policy, confirmation and audit as every
other guest action. A host working directory is not a sandbox, so none of
them ever runs code on Windows; a guest failure never falls back to the host.

**Status:** tested, not live. The guest worker's sandbox is exercised by
`Tools/Guest/test_guest_worker.py` on Linux, as root and as an unprivileged
user. The host side (tool offering, policy, approvals, patching, export) is
exercised by the guest-runtime and tool-loop suites against a fake guest. No
run has been made in a real VMware guest yet.

### Enabling

1. Start the workspace as described above
   (`RuntimeData/VirtualMachine/settings.json`).
2. In the runtime root's `RuntimeData/Capabilities/capabilities.json`, set
   `guest.enabled` and the permissions you want, and approve the interpreter:

   ```json
   "guest": { "enabled": true, "read": true, "write": true, "execute": true,
              "allowTaskExecution": false, "approvedExecutables": ["/usr/bin/python3"] }
   ```

   `run_python` needs `execute` and `/usr/bin/python3`. `share_project` needs
   `read`, `write` and at least one approved host folder. `run_tests` needs
   `execute` and its command's executable on the list.
3. Guests provisioned before this change are **not upgraded**: their worker
   does not advertise `run_python` and lacks the sandbox packages, so the
   tools are not offered and the activity view says to re-provision. Create a
   new enrollment to get the new worker, numpy, matplotlib and sympy.

Each private turn reports "Code sandbox: Ready" or "Code sandbox: Off" with
the reason (permission off, workspace not ready, old guest, or a sandbox that
could not isolate a process).

### What runs where

| Step | Where | Approval |
| --- | --- | --- |
| `run_python` | A new `sandbox/<run>` folder in the guest, deleted after the run | Every run (its own confirmation, even in owner full access) |
| `share_project` | Host reads text files from an approved folder; the guest receives copies under `projects/<name>` | Every share, showing the folder and the test command |
| `repo_grep`, `read_file_range` | Guest copy | Read-only; follows the automatic ceiling |
| `apply_patch` | Guest copy; each file's diff is the review text | Guest writes follow the ceiling (asks in supervised mode) |
| `run_tests` | Guest copy, the command approved at share time | Every run |
| `export_patch` | Reads the guest copy, writes a `.patch` under the companion's `RuntimeData/Sandbox/patches` | None needed: Revia's own folder |

Never touched on the host: your project's files (sharing reads them once;
nothing is mounted, and a patch is never applied for you), any approved root
other than reading the shared folder, and anything else outside the
companion's `RuntimeData/Sandbox`. Generated files land in
`RuntimeData/Sandbox/runs/<run>/` (the newest 20 runs are kept) after their
names, types and digests are checked; pictures must have the bytes of their
type before they are shown on the Canvas. Interpreter output reaches the
model as bounded, labelled untrusted data.

`vm_run_python` stays at the same risk as `vm_execute_process`. The guest
worker confines the run, but the host can only read that from the guest's
receipt, so it does not lower the approval on the guest's word.

### Sandbox limits (guest-side constants)

| Resource | Limit |
| --- | --- |
| Network | None: a new user and network namespace with only a loopback device that is down. If it cannot be set up, the code does not run |
| Wall clock | 1–120 s per run (default 30), within `guest.maxTimeoutMs` |
| CPU time | The wall-clock limit in seconds, then the kernel kills the run |
| Address space | 1 GiB |
| File size | 16 MiB per file |
| Processes | 64 for the user |
| Output | `guest.maxOutputBytes` (default 64 KiB); the model sees about 4 KB |
| Program size | 30,000 characters, and `guest.maxWriteBytes` |
| Files back | 4 files, 512 KiB in all: png, jpg, gif, csv, txt, json |

Sharing copies at most 2,000 text files, 8 MiB in all, each within
`guest.maxWriteBytes`; larger folders are refused rather than partly shared.
Version-control and build folders, binary files, links, and files named like
or containing credentials are left out and listed.

Limits of this isolation: a run is not jailed from the rest of the guest's
workspace (it can read projects you shared into the guest), and guest
provisioning allows unprivileged user namespaces, which Ubuntu 24.04 restricts
by default, so the worker can create the network namespace. The VM remains
the isolation boundary.

## Failure, cancellation, and resuming

Readiness requires the guest bootstrap marker, every advertised executable, pinned
SSH health, and matching environment/boot identity. A second probe runs after the
startup process job closes, so a VM lost with that job cannot establish readiness.
The lifecycle call allows up to 30 minutes overall and waits up to 600 seconds for
guest health after startup. Resource shortage or failed setup reports `Blocked`;
there is no automatic indefinite retry loop.

Launching the session again retries setup using the same enrollment. Verified
cached downloads and completed provisioning are reused; interrupted downloads
restart. An incomplete key pair, unrecorded VM/disk, changed manifest/configuration,
or failed guest package/bootstrap installation can require owner repair. Revia
preserves these artifacts instead of overwriting or resetting them. Existing guests
are not automatically upgraded when the worker source changes.

Host cancellation closes the SSH channel. The helper observes disconnect/signals,
enforces its deadline, and uses Linux subreaper tracking plus bounded descendant
cleanup, including detached process sessions. Unconfirmed cleanup or a lost receipt
cannot prove success. Unverified effects and changed boot identities block dependent
work; submitted effects are not retried automatically. A verified nonzero process
result remains available for diagnosis through guest reads, without being called a
successful test run. This does not promise containment of unlimited hostile process
creation beyond the VM boundary.

Session shutdown requests a soft stop of its owned VM. An owned incomplete setup
with no VM is already stopped; failed shutdown remains unverified. The maintenance
CLI in `Tools/Guest/vmware_ubuntu.py` exposes `doctor`, `status`, `provision`,
`ensure`, `start`, `stop`, and `request`, with a required absolute `--root` and
`--owner-id`. `provision` never powers on the VM. `doctor` proves tool availability,
not guest readiness. Keep enrollment keys, seed files, and request files private.
