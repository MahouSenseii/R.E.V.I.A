param(
    [Parameter(Mandatory = $true)][string]$BuildDirectory,
    [string[]]$Targets = @(),
    [int]$Parallel = 2,
    [int]$TimeoutSeconds = 3000,
    [Parameter(Mandatory = $true)][string]$EvidenceDirectory
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'FileHash.ps1')
. (Join-Path $PSScriptRoot 'ProcessTree.ps1')
if ($Parallel -lt 1 -or $TimeoutSeconds -lt 1) { throw 'Parallel and TimeoutSeconds must be positive.' }
$buildPath = (Resolve-Path -LiteralPath $BuildDirectory).Path
New-Item -ItemType Directory -Force -Path $EvidenceDirectory | Out-Null
$evidencePath = (Resolve-Path -LiteralPath $EvidenceDirectory).Path
$cache = Get-Content (Join-Path $buildPath 'CMakeCache.txt')
$homeEntry = $cache | Where-Object { $_ -match '^CMAKE_HOME_DIRECTORY:INTERNAL=' } | Select-Object -First 1
$buildSource = $homeEntry.Substring($homeEntry.IndexOf('=') + 1)
$sourcePath = (& git -C $buildSource rev-parse --show-toplevel 2>$null)
function SaveSourceSnapshot([string]$Suffix)
{
    if (-not $sourcePath) { return }
    & git -C $sourcePath rev-parse HEAD | Set-Content (Join-Path $evidencePath "source-head-$Suffix.txt")
    & git -C $sourcePath status --porcelain=v1 | Set-Content (Join-Path $evidencePath "source-status-$Suffix.txt")
    & git -c core.safecrlf=false -C $sourcePath diff --binary | Set-Content (Join-Path $evidencePath "source-$Suffix.diff")
    $files = & git -C $sourcePath ls-files --cached --others --exclude-standard
    $manifest = @($files | Where-Object { $_ -match '^(Public|Private|Desktop|Tests|Config|Tools|Assets)/|^CMakeLists.txt$|^\.github/workflows/build-and-test.yml$' } | Sort-Object | ForEach-Object {
        $filePath = Join-Path $sourcePath $_
        if (Test-Path -LiteralPath $filePath -PathType Leaf) { [ordered]@{ path = $_; sha256 = (Get-ReviaFileHash -LiteralPath $filePath).Hash } }
    })
    $manifest | ConvertTo-Json -Depth 3 | Set-Content (Join-Path $evidencePath "source-inputs-$Suffix.json")
}
SaveSourceSnapshot 'start'
$arguments = @('--build', $buildPath, '--parallel', "$Parallel")
if ($Targets.Count -gt 0) { $arguments += @('--target') + $Targets }
$quotedArguments = $arguments | ForEach-Object { '"' + $_.Replace('"', '\"') + '"' }
$command = @('cmake') + $arguments
$started = [DateTime]::UtcNow
$watch = [Diagnostics.Stopwatch]::StartNew()
$env:NINJA_STATUS = '[%f/%t elapsed=%es] '
$exitCode = 1
$timedOut = $false
$peakWorkingBytes = 0L
$peakPrivateBytes = 0L
$peakProcesses = 0
$observed = @{}
$process = Start-Process -FilePath (Get-Command cmake -CommandType Application | Select-Object -First 1).Source -ArgumentList $quotedArguments -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $evidencePath 'build.stdout.log') -RedirectStandardError (Join-Path $evidencePath 'build.stderr.log')
try
{
    while (-not $process.HasExited)
    {
        $snapshot = @(Get-CimInstance Win32_Process)
        $capturedUtc = [DateTime]::UtcNow
        $descendants = @(Get-ReviaProcessTree $snapshot $process.Id $process.StartTime.ToUniversalTime())
        $workingBytes = 0L
        $privateBytes = 0L
        $samples = @()
        foreach ($entry in $descendants)
        {
            $workingBytes += [long]$entry.WorkingSetSize
            $privateBytes += [long]$entry.PrivatePageCount
            $sample = [ordered]@{ pid = $entry.ProcessId; parent = $entry.ParentProcessId; createdUtc = ([DateTime]$entry.CreationDate).ToUniversalTime().ToString('o'); name = $entry.Name; workingBytes = $entry.WorkingSetSize; privateBytes = $entry.PrivatePageCount; kernel100ns = $entry.KernelModeTime; user100ns = $entry.UserModeTime; command = $entry.CommandLine }
            $samples += $sample
            $observed["$($entry.ProcessId)/$($sample.createdUtc)"] = $sample
        }
        $peakWorkingBytes = [Math]::Max($peakWorkingBytes, $workingBytes)
        $peakPrivateBytes = [Math]::Max($peakPrivateBytes, $privateBytes)
        $peakProcesses = [Math]::Max($peakProcesses, $samples.Count)
        [ordered]@{ capturedUtc = $capturedUtc.ToString('o'); elapsedSeconds = $watch.Elapsed.TotalSeconds; workingBytes = $workingBytes; privateBytes = $privateBytes; processes = $samples } | ConvertTo-Json -Depth 5 -Compress | Add-Content (Join-Path $evidencePath 'resources.jsonl')
        if ($watch.Elapsed.TotalSeconds -ge $TimeoutSeconds)
        {
            $timedOut = $true
            & taskkill.exe /PID $process.Id /T /F | Out-File (Join-Path $evidencePath 'timeout.log')
            $process.WaitForExit()
            break
        }
        Start-Sleep -Seconds 2
        $process.Refresh()
    }
    $process.WaitForExit()
    $exitCode = if ($timedOut) { 124 } else { $process.ExitCode }
}
finally
{
    $watch.Stop()
    SaveSourceSnapshot 'end'
    $sourceChanged = $false
    if ($sourcePath) { $sourceChanged = (Get-ReviaFileHash (Join-Path $evidencePath 'source-inputs-start.json')).Hash -ne (Get-ReviaFileHash (Join-Path $evidencePath 'source-inputs-end.json')).Hash }
    [ordered]@{ command = $command; startedUtc = $started.ToString('o'); finishedUtc = [DateTime]::UtcNow.ToString('o'); elapsedSeconds = $watch.Elapsed.TotalSeconds; exitCode = $exitCode; timedOut = $timedOut; sourceChangedDuringBuild = $sourceChanged; parallel = $Parallel; samplePeriodSeconds = 2; samplingScope = 'instantaneous creation-matched process tree'; peakWorkingBytes = $peakWorkingBytes; peakPrivateBytes = $peakPrivateBytes; peakProcessCount = $peakProcesses; observedProcesses = @($observed.Values) } | ConvertTo-Json -Depth 6 | Set-Content (Join-Path $evidencePath 'build-result.json')
    foreach ($name in @('.ninja_log', 'CMakeCache.txt', 'compile_commands.json', 'build.ninja'))
    {
        $path = Join-Path $buildPath $name
        if (Test-Path -LiteralPath $path) { Copy-Item -LiteralPath $path -Destination $evidencePath }
    }
    Get-Content (Join-Path $evidencePath 'build.stdout.log') -Tail 12
    Get-Content (Join-Path $evidencePath 'build.stderr.log') -Tail 12
    Write-Output "Build exit=$exitCode elapsed=$($watch.Elapsed.TotalSeconds)s peakWorkingBytes=$peakWorkingBytes peakPrivateBytes=$peakPrivateBytes timedOut=$timedOut"
}
exit $exitCode
