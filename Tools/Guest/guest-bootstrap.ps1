$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Fixed guest paths prevent a workspace file from supplying a host launch command.
$guestControl = 'C:\Revia\Control'
$guestInput = 'C:\Revia\Input'
$guestArtifacts = 'C:\Revia\Artifacts'
$guestWork = 'C:\Revia\Work'
$contract = Get-Content -LiteralPath (Join-Path $guestControl 'guest-contract.json') -Raw | ConvertFrom-Json
if ($contract.schemaVersion -ne 1 -or $contract.runId -notmatch '^[a-f0-9-]{36}$' -or
    $contract.inputDirectory -ne $guestInput -or $contract.artifactDirectory -ne $guestArtifacts)
{
    throw 'The guest bootstrap contract is invalid.'
}
if ($env:USERNAME -ne 'WDAGUtilityAccount' -or $env:COMPUTERNAME -eq $contract.hostComputerName)
{
    throw 'This bootstrap must be invoked inside the selected Windows Sandbox guest.'
}
if (-not (Test-Path -LiteralPath $guestInput -PathType Container) -or
    -not (Test-Path -LiteralPath $guestArtifacts -PathType Container))
{
    throw 'The selected guest mounts are unavailable.'
}
[void][System.IO.Directory]::CreateDirectory($guestWork)
$receipt = [ordered]@{
    schemaVersion = 1
    runId = $contract.runId
    state = 'bootstrap_completed'
    observedAt = [DateTime]::UtcNow.ToString('o')
    guestAccount = $env:USERNAME
    inputMounted = $true
    artifactWritable = $true
    workDirectory = $guestWork
    vmIsolationVerified = $false
    guestControlReady = $false
}
$bytes = [System.Text.Encoding]::UTF8.GetBytes(($receipt | ConvertTo-Json -Depth 5))
$stream = [System.IO.File]::Open((Join-Path $guestArtifacts 'guest-receipt.json'),
    [System.IO.FileMode]::CreateNew, [System.IO.FileAccess]::Write, [System.IO.FileShare]::Read)
try
{
    $stream.Write($bytes, 0, $bytes.Length)
    $stream.Flush($true)
}
finally
{
    $stream.Dispose()
}
