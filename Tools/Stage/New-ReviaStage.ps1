<#
.SYNOPSIS
Creates the Revia-Stage Hyper-V guest: a Windows machine where her desktop actions run.

.DESCRIPTION
The stage holds her hands and nothing else: no model, no memory, no credentials, no
host folders. It gets its own private virtual switch (no route to the host's LAN unless
you add one), a clean checkpoint to restore before each session, and the guest program
(ReviaStageGuest.exe) serving the stage channel on the port the host's capability file
names. Run as Administrator with Hyper-V enabled.

GPU: Microsoft does not support GPU paravirtualization on client Windows, and the
community route (Easy-GPU-PV, archived) has no Vulkan, flaky OpenGL and needs a dummy
display. This script creates a plain guest with no GPU. A stage that must render a game
needs that spike-tested separately; nothing here promises it.

.EXAMPLE
.\New-ReviaStage.ps1 -Name ReviaStage -Vhdx C:\VMs\revia-stage.vhdx -MemoryGB 8 -Cpus 4
#>
[CmdletBinding()]
param(
    [string]$Name = "ReviaStage",
    [Parameter(Mandatory = $true)][string]$Vhdx,
    [int]$MemoryGB = 8,
    [int]$Cpus = 4,
    [string]$SwitchName = "ReviaStagePrivate",
    [string]$CheckpointName = "clean"
)
$ErrorActionPreference = "Stop"
if (-not (Get-Command Get-VM -ErrorAction SilentlyContinue)) {
    throw "Hyper-V PowerShell is not available. Enable Hyper-V (Windows Features) and run as Administrator."
}
if (-not (Test-Path $Vhdx)) {
    throw "The VHDX does not exist: $Vhdx. Install Windows into a VHDX first (or export one from an existing VM)."
}
if (-not (Get-VMSwitch -Name $SwitchName -ErrorAction SilentlyContinue)) {
    # Private: the guest talks to the host over the stage channel only when you attach
    # an internal adapter or use Hyper-V sockets; it never sees the LAN by default.
    New-VMSwitch -Name $SwitchName -SwitchType Private | Out-Null
    Write-Host "Created private switch $SwitchName."
}
if (Get-VM -Name $Name -ErrorAction SilentlyContinue) {
    Write-Host "The VM $Name already exists; leaving it as it is."
} else {
    New-VM -Name $Name -MemoryStartupBytes ($MemoryGB * 1GB) -Generation 2 -VHDPath $Vhdx -SwitchName $SwitchName | Out-Null
    Set-VM -Name $Name -ProcessorCount $Cpus -AutomaticCheckpointsEnabled $false -CheckpointType Standard
    # No host folders, no clipboard, no shared devices: the guest is disposable.
    Set-VM -Name $Name -EnhancedSessionTransportType HvSocket -ErrorAction SilentlyContinue
    Disable-VMIntegrationService -VMName $Name -Name "Guest Service Interface" -ErrorAction SilentlyContinue
    Write-Host "Created $Name with $MemoryGB GB, $Cpus CPUs, on $SwitchName."
}
if (-not (Get-VMCheckpoint -VMName $Name -Name $CheckpointName -ErrorAction SilentlyContinue)) {
    Checkpoint-VM -Name $Name -SnapshotName $CheckpointName
    Write-Host "Took checkpoint '$CheckpointName'. Restore it before each session with Restore-ReviaStage.ps1."
}
Write-Host ""
Write-Host "Inside the guest: copy ReviaStageGuest.exe and a capabilities.json, then run"
Write-Host "  ReviaStageGuest.exe --capabilities capabilities.json --host 0.0.0.0 --port 39610 --tier 1"
Write-Host "On the host: set stage.enabled to true and stage.host to the guest's address in Config\capabilities.json."
