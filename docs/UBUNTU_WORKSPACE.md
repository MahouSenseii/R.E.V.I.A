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
`vm_execute_process` are distinct from host actions. File paths are relative POSIX
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
