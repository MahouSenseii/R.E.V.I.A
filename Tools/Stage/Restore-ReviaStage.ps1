<#
.SYNOPSIS
Puts the stage back to its clean checkpoint and starts it: what runs before a session.
#>
[CmdletBinding()]
param(
    [string]$Name = "ReviaStage",
    [string]$CheckpointName = "clean"
)
$ErrorActionPreference = "Stop"
$vm = Get-VM -Name $Name
if ($vm.State -ne "Off") { Stop-VM -Name $Name -TurnOff -Force }
Restore-VMCheckpoint -VMName $Name -Name $CheckpointName -Confirm:$false
Start-VM -Name $Name
Write-Host "$Name restored to '$CheckpointName' and started. Whatever the last session did is gone."
