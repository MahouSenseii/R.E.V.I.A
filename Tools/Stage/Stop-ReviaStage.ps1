<#
.SYNOPSIS
The kill switch: turns the stage off at once, from the host, whatever it is doing.

.DESCRIPTION
Stop-VM -TurnOff is a power cut, not a shutdown request: nothing in the guest can delay
it or ask. Bind this to a hotkey. With -Restore the clean checkpoint is put back too.
#>
[CmdletBinding()]
param(
    [string]$Name = "ReviaStage",
    [switch]$Restore,
    [string]$CheckpointName = "clean"
)
$ErrorActionPreference = "Stop"
Stop-VM -Name $Name -TurnOff -Force -ErrorAction SilentlyContinue
Write-Host "$Name is off."
if ($Restore) {
    Restore-VMCheckpoint -VMName $Name -Name $CheckpointName -Confirm:$false
    Write-Host "Restored '$CheckpointName'."
}
