[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ProjectRoot,
    [Parameter(Mandatory = $true)][string]$Path,
    [Parameter(Mandatory = $true)][string]$TokenFile,
    [uri]$Endpoint = 'http://127.0.0.1:17865/v1/editor/observations',
    [ValidateRange(500, 1500)][int]$DebounceMilliseconds = 750
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$taskRoot = [IO.Path]::GetFullPath($ProjectRoot)
if ($taskRoot.Length -gt [IO.Path]::GetPathRoot($taskRoot).Length) {
    $taskRoot = $taskRoot.TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
}
$taskPath = [IO.Path]::GetFullPath($Path)
$taskPrefix = $taskRoot.TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
if (-not [IO.Directory]::Exists($taskRoot) -or -not [IO.File]::Exists($taskPath) -or
    -not $taskPath.StartsWith($taskPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Choose one existing saved file inside the explicit project root.'
}
if (-not $Endpoint.IsAbsoluteUri -or $Endpoint.Scheme -ne 'http' -or
    $Endpoint.Host -notin @('127.0.0.1', 'localhost') -or $Endpoint.Port -lt 1024 -or
    $Endpoint.AbsolutePath -ne '/v1/editor/observations' -or $Endpoint.UserInfo -or $Endpoint.Query -or $Endpoint.Fragment) {
    throw 'The editor endpoint must be the plain loopback HTTP editor-observation route.'
}

function Get-SavedRevision {
    if (-not [IO.File]::Exists($taskPath) -or (Get-Item -LiteralPath $taskPath).Length -gt 16384) { return $null }
    $taskStream = [IO.File]::Open($taskPath, [IO.FileMode]::Open, [IO.FileAccess]::Read,
        ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    $taskData = New-Object byte[] 16385
    $taskRead = 0
    try {
        while ($taskRead -lt $taskData.Length) {
            $taskMore = $taskStream.Read($taskData, $taskRead, $taskData.Length - $taskRead)
            if ($taskMore -eq 0) { break }
            $taskRead += $taskMore
        }
    }
    finally { $taskStream.Dispose() }
    if ($taskRead -gt 16384) { return $null }
    $taskHash = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($taskHash.ComputeHash($taskData, 0, $taskRead)).Replace('-', '').ToLowerInvariant() }
    finally { $taskHash.Dispose() }
}

# Establish a baseline without publishing an initial event or reading an unsaved buffer.
$taskLastRevision = Get-SavedRevision
if ($null -eq $taskLastRevision) { throw 'The saved file must be no larger than 16 KiB.' }
$taskWatcher = New-Object IO.FileSystemWatcher([IO.Path]::GetDirectoryName($taskPath), [IO.Path]::GetFileName($taskPath))
$taskWatcher.IncludeSubdirectories = $false
$taskWatcher.NotifyFilter = [IO.NotifyFilters]::LastWrite -bor [IO.NotifyFilters]::Size -bor [IO.NotifyFilters]::FileName
$taskWatcher.InternalBufferSize = 4096
$taskIds = @()
$taskPending = $false
$taskSuppressedRevision = $null
$taskRetryCount = 0
$taskDelay = $DebounceMilliseconds
$taskQuiet = [Diagnostics.Stopwatch]::StartNew()
try {
    foreach ($taskKind in @('Changed', 'Created', 'Renamed', 'Error')) {
        $taskId = 'revia-editor-' + [Guid]::NewGuid().ToString('N')
        $null = Register-ObjectEvent -InputObject $taskWatcher -EventName $taskKind -SourceIdentifier $taskId
        $taskIds += $taskId
    }
    $taskWatcher.EnableRaisingEvents = $true
    Write-Host 'Watching one saved file. No initial observation is sent. Press Ctrl+C to stop.'
    while ($true) {
        foreach ($taskId in $taskIds) {
            foreach ($taskEvent in @(Get-Event -SourceIdentifier $taskId -ErrorAction SilentlyContinue)) {
                try {
                    if ($taskEvent.SourceEventArgs -is [IO.ErrorEventArgs]) {
                        Write-Warning 'The saved-file watcher lost events; no contents were inferred. Restart it or send the file explicitly.'
                    }
                    elseif ([string]::Equals($taskEvent.SourceEventArgs.FullPath, $taskPath, [StringComparison]::OrdinalIgnoreCase)) {
                        $taskPending = $true
                        $taskRetryCount = 0
                        $taskDelay = $DebounceMilliseconds
                        $taskQuiet.Restart()
                    }
                }
                finally { Remove-Event -EventIdentifier $taskEvent.EventIdentifier -ErrorAction SilentlyContinue }
            }
        }
        if ($taskPending -and $taskQuiet.ElapsedMilliseconds -ge $taskDelay) {
            $taskPending = $false
            $taskRevision = $null
            try {
                $taskRevision = Get-SavedRevision
                if ($null -ne $taskRevision -and $taskRevision -ne $taskLastRevision -and $taskRevision -ne $taskSuppressedRevision) {
                    $taskResult = & (Join-Path $PSScriptRoot 'Send-EditorObservation.ps1') -ProjectRoot $taskRoot -Path $taskPath -TokenFile $TokenFile -Endpoint $Endpoint
                    if ($taskResult.accepted) {
                        $taskLastRevision = $taskResult.revision
                        $taskRetryCount = 0
                    }
                    elseif ($taskResult.retryable -and $taskRetryCount -lt 3) {
                        ++$taskRetryCount
                        $taskDelay = 1000 * [math]::Pow(2, $taskRetryCount - 1)
                        $taskPending = $true
                        $taskQuiet.Restart()
                    }
                    else { $taskSuppressedRevision = $taskResult.revision }
                    Write-Output $taskResult
                }
            }
            catch {
                # Uncertain sends and policy refusals are never automatically replayed.
                $taskSuppressedRevision = $taskRevision
                Write-Warning 'The saved-file observation was not delivered. Check local API, awareness, pause and approved-root settings.'
            }
        }
        Start-Sleep -Milliseconds 100
    }
}
finally {
    $taskWatcher.EnableRaisingEvents = $false
    foreach ($taskId in $taskIds) {
        Unregister-Event -SourceIdentifier $taskId -ErrorAction SilentlyContinue
        Get-Event -SourceIdentifier $taskId -ErrorAction SilentlyContinue | Remove-Event -ErrorAction SilentlyContinue
    }
    $taskWatcher.Dispose()
    $taskQuiet.Stop()
}
