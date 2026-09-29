# Revia-Stage: her hands in a guest machine

The design keeps her brain, memory, policy, approvals and presence on the host, and puts
her hands -- the desktop executors, screenshots, a browser, game adapters -- in a
Hyper-V guest she drives over a private channel. A runaway action then costs a
checkpoint, not the machine you live on.

What exists:

- **The channel** (`Stage/stageProtocol.h`, `Stage/stageChannel.h`): JSON lines over
  TCP between the host and the guest. The host sends typed desktop actions its policy
  already admitted (and, where the policy said so, the person confirmed); the guest
  performs and reports. Four tiers, enforced at both ends: T0 observe, T1 act inside an
  approved application, T2 the guest's whole desktop, T3 start processes. The host will
  not send above the tier the owner granted (`stage.grantedTier` in `capabilities.json`),
  the guest will not perform above the tier it was started with (`--tier`), and the
  guest decides the tier a request needs from the request itself rather than taking the
  host's word. A `halt` message stops the guest for good until it is restarted.
- **The host side** (`Stage/stageActionExecutor.h`): with `stage.enabled`, every desktop
  action goes to the guest and none runs on the host. Same policy, same confirmations,
  same rate limits, same audit log; the executor is the only thing that changes.
- **The guest program** (`Stage/reviaStageGuest.cpp`, target `ReviaStageGuest`, Windows
  only): the same Windows executors the host uses, behind the same guard and approval
  gate, reading their own `capabilities.json` inside the guest, serving the channel.
- **The scripts**: `New-ReviaStage.ps1` creates the VM on a private switch with a clean
  checkpoint and no host folders; `Restore-ReviaStage.ps1` puts the checkpoint back and
  starts it before a session; `Stop-ReviaStage.ps1` is the kill switch (`Stop-VM
  -TurnOff`, a power cut nothing in the guest can delay), with `-Restore` to reset too.

What is tested: the protocol both ways, the host client and guest server over a real
loopback socket with a fake executor, the tiers at both ends, halt, timeouts and lost
connections, and the runtime routing a desktop action to the guest when the stage is
on (`ReviaTests --stage`).

What is not: the guest program has not been built or run in a real guest (it is
Windows-only wiring of executors that are tested on the host); no Hyper-V VM has been
created with these scripts here; the channel runs over TCP on the private switch, and
the Hyper-V socket transport (which needs no network at all) is not written. GPU
paravirtualization is unsupported on client Windows, so a stage that must render a game
needs its own spike first; Windows Sandbox is the lighter, non-persistent alternative for
a disposable risky task.

## Set up

1. Install Windows into a VHDX (or export one), then on the host as Administrator:
   `.\Tools\Stage\New-ReviaStage.ps1 -Vhdx C:\VMs\revia-stage.vhdx`.
2. In the guest: copy `ReviaStageGuest.exe` and a `capabilities.json` that approves the
   applications she may drive there; run
   `ReviaStageGuest.exe --capabilities capabilities.json --host 0.0.0.0 --port 39610 --tier 1`.
   Give the guest an address the host can reach (an internal switch, or a second
   adapter) and open the port in its firewall for the host only.
3. On the host, in `Config\capabilities.json`:
   `"stage": {"enabled": true, "host": "<guest address>", "port": 39610, "grantedTier": 1, "vmName": "ReviaStage"}`.
4. Before each session: `.\Tools\Stage\Restore-ReviaStage.ps1`. To stop everything at
   once: `.\Tools\Stage\Stop-ReviaStage.ps1`.
