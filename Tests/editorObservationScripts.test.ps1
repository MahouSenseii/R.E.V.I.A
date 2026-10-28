[CmdletBinding()]
param([string]$NativeTokenFile = '', [switch]$NativeTokenOnly)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($NativeTokenOnly -and -not $NativeTokenFile) { throw 'The native-token-only control requires an explicit native token file.' }
Add-Type -AssemblyName System.Security
$taskDeadline = [DateTime]::UtcNow.AddSeconds(80)
$taskRepository = Split-Path -Parent $PSScriptRoot
$taskSender = Join-Path $taskRepository 'Tools/Presence/Send-EditorObservation.ps1'
$taskWatcherScript = Join-Path $taskRepository 'Tools/Presence/Watch-EditorFile.ps1'
$taskPowerShell = Join-Path $env:WINDIR 'System32/WindowsPowerShell/v1.0/powershell.exe'
$taskTempParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$taskLeaf = 'revia-editor-scripts-' + [Guid]::NewGuid().ToString('N')
$taskRoot = Join-Path $taskTempParent $taskLeaf
$null = New-Item -ItemType Directory -Path $taskRoot
$taskProject = Join-Path $taskRoot 'project'
$null = New-Item -ItemType Directory -Path $taskProject
$taskFile = Join-Path $taskProject 'exact.cpp'
$taskOther = Join-Path $taskProject 'other.cpp'
$taskTokenFile = Join-Path $taskRoot 'protected-token'
$taskUtf8 = New-Object Text.UTF8Encoding($false, $true)
$taskModePath = Join-Path $taskRoot 'mode'
$taskWatcher = $null
$taskServer = $null
$taskNativeBytes = $null
$script:taskLastSenderOutput = ''
$taskSecrets = @(('revia-' + ('1' * 64)), ('revia-' + ('2' * 64)), ('revia-' + ('A' * 64)))

function Assert-Condition([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
    if ([DateTime]::UtcNow -ge $taskDeadline) { throw 'The editor-script fixture exceeded its 80-second work budget.' }
}
function Set-SavedText([string]$Text) { [IO.File]::WriteAllText($taskFile, $Text, $taskUtf8) }
function Save-ProtectedToken([string]$Name, [byte[]]$Bytes) {
    try {
        $taskProtected = [Security.Cryptography.ProtectedData]::Protect($Bytes, $null,
            [Security.Cryptography.DataProtectionScope]::CurrentUser)
        [IO.File]::WriteAllBytes((Join-Path $taskRoot $Name), $taskProtected)
    }
    finally { [Array]::Clear($Bytes, 0, $Bytes.Length) }
}
function Get-Digest([string]$Path) {
    $taskHash = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($taskHash.ComputeHash([IO.File]::ReadAllBytes($Path))).Replace('-', '').ToLowerInvariant() }
    finally { $taskHash.Dispose() }
}
function Get-Receipts {
    Get-ChildItem -LiteralPath $taskRoot -Filter 'request-*.json' -File | Sort-Object Name |
        ForEach-Object { [IO.File]::ReadAllText($_.FullName) | ConvertFrom-Json }
}
function Get-AuthorizedCount { return @(Get-Receipts | Where-Object { $_.authorized }).Count }
function Wait-AuthorizedCount([int]$Count, [int]$Seconds = 6) {
    $taskUntil = [DateTime]::UtcNow.AddSeconds($Seconds)
    while ((Get-AuthorizedCount) -lt $Count -and [DateTime]::UtcNow -lt $taskUntil) {
        Assert-Condition ([DateTime]::UtcNow -lt $taskDeadline) 'The editor-script fixture exceeded its time budget.'
        Start-Sleep -Milliseconds 100
    }
    Assert-Condition ((Get-AuthorizedCount) -eq $Count) 'Unexpected authorized editor-observation count.'
}
function Quote-ProcessArgument([string]$Value) {
    if ($Value.Contains('"') -or $Value.Contains("`r") -or $Value.Contains("`n") -or $Value.EndsWith('\')) {
        throw 'The fixture received an unsupported process argument.'
    }
    return '"' + $Value + '"'
}
function Start-Script([string]$Script, [hashtable]$Overrides = @{}) {
    $taskValues = [ordered]@{ ProjectRoot = $taskProject; Path = $taskFile; TokenFile = $taskTokenFile; Endpoint = $taskEndpoint }
    foreach ($taskKey in $Overrides.Keys) { $taskValues[$taskKey] = $Overrides[$taskKey] }
    $taskArguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $Script)
    foreach ($taskKey in $taskValues.Keys) { $taskArguments += @(('-' + $taskKey), ([string]$taskValues[$taskKey])) }
    $taskStart = New-Object Diagnostics.ProcessStartInfo
    $taskStart.FileName = $taskPowerShell
    $taskStart.Arguments = ($taskArguments | ForEach-Object { Quote-ProcessArgument $_ }) -join ' '
    $taskStart.UseShellExecute = $false
    $taskStart.CreateNoWindow = $true
    $taskStart.RedirectStandardOutput = $true
    $taskStart.RedirectStandardError = $true
    $taskProcess = New-Object Diagnostics.Process
    $taskProcess.StartInfo = $taskStart
    $null = $taskProcess.Start()
    return [pscustomobject]@{ Process = $taskProcess; Output = $taskProcess.StandardOutput.ReadToEndAsync(); Error = $taskProcess.StandardError.ReadToEndAsync() }
}
function Assert-NoDisclosure([string]$Text) {
    foreach ($taskSecret in $taskSecrets) { Assert-Condition (-not $Text.Contains($taskSecret)) 'A script disclosed a bearer token.' }
}
function Run-Sender([hashtable]$Overrides = @{}) {
    $taskRun = Start-Script $taskSender $Overrides
    try {
        $taskRemaining = [int][Math]::Max(1, [Math]::Min(10000, ($taskDeadline - [DateTime]::UtcNow).TotalMilliseconds))
        if (-not $taskRun.Process.WaitForExit($taskRemaining)) { $taskRun.Process.Kill(); throw 'The sender fixture exceeded its time budget.' }
        $taskOutput = $taskRun.Output.GetAwaiter().GetResult() + $taskRun.Error.GetAwaiter().GetResult()
        Assert-NoDisclosure $taskOutput
        $script:taskLastSenderOutput = $taskOutput
        return $taskRun.Process.ExitCode
    }
    finally { if (-not $taskRun.Process.HasExited) { $taskRun.Process.Kill() }; $taskRun.Process.Dispose() }
}

try {
    Set-SavedText "int saved = 1;`n"
    [IO.File]::WriteAllText($taskOther, "unrelated`n", $taskUtf8)
    [IO.File]::WriteAllText($taskModePath, 'accept')
    Save-ProtectedToken 'protected-token' ([Text.Encoding]::ASCII.GetBytes($taskSecrets[0]))
    Save-ProtectedToken 'wrong-token' ([Text.Encoding]::ASCII.GetBytes($taskSecrets[1]))
    Save-ProtectedToken 'uppercase-token' ([Text.Encoding]::ASCII.GetBytes($taskSecrets[2]))
    Save-ProtectedToken 'newline-token' ([Text.Encoding]::ASCII.GetBytes($taskSecrets[0] + "`n"))
    Save-ProtectedToken 'invalid-utf8-token' ([byte[]]@(255, 254, 253))
    $taskProtectedDigest = Get-Digest $taskTokenFile
    $taskCorrupt = [IO.File]::ReadAllBytes($taskTokenFile)
    $taskCorrupt[[int]($taskCorrupt.Length / 2)] = $taskCorrupt[[int]($taskCorrupt.Length / 2)] -bxor 128
    [IO.File]::WriteAllBytes((Join-Path $taskRoot 'corrupt-token'), $taskCorrupt)
    [IO.File]::WriteAllBytes((Join-Path $taskRoot 'empty-token'), [byte[]]@())
    [IO.File]::WriteAllBytes((Join-Path $taskRoot 'oversize-token'), (New-Object byte[] 4097))
    [IO.File]::WriteAllText((Join-Path $taskRoot 'plaintext-token'), $taskSecrets[0], $taskUtf8)
    $taskNativePath = ''
    if ($NativeTokenFile) {
        $taskNativePath = [IO.Path]::GetFullPath($NativeTokenFile)
        Assert-Condition ([IO.File]::Exists($taskNativePath) -and (Get-Item -LiteralPath $taskNativePath).Length -le 4096) 'The optional native token file is unavailable or oversized.'
        $taskNativeBytes = [IO.File]::ReadAllBytes($taskNativePath)
        $taskPlain = [Security.Cryptography.ProtectedData]::Unprotect($taskNativeBytes, $null, [Security.Cryptography.DataProtectionScope]::CurrentUser)
        try {
            $taskNativeToken = $taskUtf8.GetString($taskPlain)
            Assert-Condition ($taskNativeToken.Length -eq 70 -and $taskNativeToken -cmatch '\Arevia-[0-9a-f]{64}\z') 'The optional native token has an invalid format.'
            $taskSecrets += $taskNativeToken
        }
        finally { [Array]::Clear($taskPlain, 0, $taskPlain.Length) }
    }
    $taskServer = Start-Job -ArgumentList $taskRoot, $taskNativePath -ScriptBlock {
        param($FixtureRoot, $NativePath)
        $ErrorActionPreference = 'Stop'
        Add-Type -AssemblyName System.Security
        $expected = @('Bearer revia-' + ('1' * 64))
        if ($NativePath) {
            $plain = [Security.Cryptography.ProtectedData]::Unprotect([IO.File]::ReadAllBytes($NativePath), $null, [Security.Cryptography.DataProtectionScope]::CurrentUser)
            try { $expected += 'Bearer ' + [Text.Encoding]::UTF8.GetString($plain) }
            finally { [Array]::Clear($plain, 0, $plain.Length) }
        }
        $listener = New-Object Net.Sockets.TcpListener([Net.IPAddress]::Loopback, 0)
        $listener.Start()
        [IO.File]::WriteAllText((Join-Path $FixtureRoot 'port'), [string]$listener.LocalEndpoint.Port)
        $until = [DateTime]::UtcNow.AddSeconds(82)
        $number = 0
        try {
            while ([DateTime]::UtcNow -lt $until) {
                if (-not $listener.Pending()) { Start-Sleep -Milliseconds 10; continue }
                $client = $listener.AcceptTcpClient()
                try {
                    $stream = $client.GetStream(); $stream.ReadTimeout = 2000; $stream.WriteTimeout = 2000
                    $header = New-Object Collections.Generic.List[byte]
                    while ($header.Count -lt 16384) {
                        $byte = $stream.ReadByte(); if ($byte -lt 0) { throw 'Incomplete fixture request.' }
                        $header.Add([byte]$byte)
                        if ($header.Count -ge 4 -and [Text.Encoding]::ASCII.GetString($header.ToArray(), $header.Count - 4, 4) -eq "`r`n`r`n") { break }
                    }
                    $lines = [Text.Encoding]::ASCII.GetString($header.ToArray()) -split "`r`n"
                    if ($lines[0] -ne 'POST /v1/editor/observations HTTP/1.1') { throw 'Unexpected fixture route.' }
                    $headers = @{}
                    foreach ($line in $lines | Select-Object -Skip 1) { $colon = $line.IndexOf(':'); if ($colon -gt 0) { $headers[$line.Substring(0, $colon)] = $line.Substring($colon + 1).Trim() } }
                    $length = 0
                    if (-not [int]::TryParse($headers['Content-Length'], [ref]$length) -or $length -lt 1 -or $length -gt 8192) { throw 'Unbounded fixture body.' }
                    $bytes = New-Object byte[] $length; $read = 0
                    while ($read -lt $length) { $more = $stream.Read($bytes, $read, $length - $read); if ($more -eq 0) { throw 'Incomplete fixture body.' }; $read += $more }
                    $body = [Text.Encoding]::UTF8.GetString($bytes) | ConvertFrom-Json
                    $authorized = $false
                    foreach ($key in $expected) { if ($headers['Authorization'] -ceq $key) { $authorized = $true } }
                    $mode = [IO.File]::ReadAllText((Join-Path $FixtureRoot 'mode'))
                    if ($mode -eq 'busy_once' -and $authorized) { [IO.File]::WriteAllText((Join-Path $FixtureRoot 'mode'), 'accept') }
                    $accepted = $authorized -and $mode -eq 'accept'
                    $retryable = $authorized -and $mode -in @('busy_once', 'busy_always')
                    $status = if (-not $authorized) { 401 } elseif ($accepted) { 202 } else { 409 }
                    ++$number
                    $receipt = @{ authorized = $authorized; status = $status; body = $body; retryable = $retryable } | ConvertTo-Json -Depth 5 -Compress
                    $partial = Join-Path $FixtureRoot 'request.partial'
                    [IO.File]::WriteAllText($partial, $receipt)
                    [IO.File]::Move($partial, (Join-Path $FixtureRoot ('request-{0:D4}.json' -f $number)))
                    $response = @{ accepted = $accepted; reason = 'fixture result'; eventId = if ($accepted) { [string]$number } else { '' }; retryable = $retryable } | ConvertTo-Json -Compress
                    $responseBytes = [Text.Encoding]::UTF8.GetBytes($response)
                    $responseHead = [Text.Encoding]::ASCII.GetBytes("HTTP/1.1 $status Fixture`r`nContent-Type: application/json`r`nContent-Length: $($responseBytes.Length)`r`nConnection: close`r`n`r`n")
                    $stream.Write($responseHead, 0, $responseHead.Length); $stream.Write($responseBytes, 0, $responseBytes.Length)
                }
                finally { $client.Dispose() }
            }
        }
        finally { $listener.Stop() }
    }
    $taskReady = [DateTime]::UtcNow.AddSeconds(8)
    while (-not [IO.File]::Exists((Join-Path $taskRoot 'port')) -and [DateTime]::UtcNow -lt $taskReady) { Start-Sleep -Milliseconds 100 }
    Assert-Condition ([IO.File]::Exists((Join-Path $taskRoot 'port'))) 'The loopback fixture did not start.'
    $taskPort = [IO.File]::ReadAllText((Join-Path $taskRoot 'port'))
    $taskEndpoint = 'http://127.0.0.1:' + $taskPort + '/v1/editor/observations'
    $taskSenderExit = Run-Sender
    if ($taskSenderExit -ne 0) {
        $taskDiagnostic = $script:taskLastSenderOutput + ($taskServer.ChildJobs[0].Error | Out-String)
        foreach ($taskSecret in $taskSecrets) { $taskDiagnostic = $taskDiagnostic.Replace($taskSecret, '[redacted]') }
        throw ('The protected-token sender fixture failed: ' + $taskDiagnostic.Substring(0, [Math]::Min(2400, $taskDiagnostic.Length)))
    }
    Wait-AuthorizedCount 1
    $taskBody = @(Get-Receipts)[0].body
    Assert-Condition (@($taskBody.PSObject.Properties.Name).Count -eq 5 -and $taskBody.source -eq 'saved_file' -and
        $taskBody.projectRoot -eq $taskProject -and $taskBody.path -eq $taskFile -and $taskBody.revision -eq (Get-Digest $taskFile) -and
        [Math]::Abs($taskBody.occurredAtUnixMs - [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()) -lt 10000 -and
        -not ($taskBody | ConvertTo-Json).Contains('int saved')) 'The sender did not report exact saved bytes as bounded metadata only.'
    Write-Output 'PASS protected token, exact saved-byte digest, timestamp and metadata-only payload'
    $taskBase = 1
    if ($taskNativePath) {
        Assert-Condition ((Run-Sender @{ TokenFile = $taskNativePath }) -eq 0) 'The sender rejected the native-owned DPAPI token.'
        ++$taskBase; Wait-AuthorizedCount $taskBase
        Write-Output ('PASS read-only native DPAPI token compatibility ({0} protected bytes)' -f $taskNativeBytes.Length)
        if ($NativeTokenOnly) { return }
    }
    Assert-Condition ((Run-Sender @{ TokenFile = (Join-Path $taskRoot 'wrong-token') }) -ne 0) 'A wrong protected bearer was accepted.'
    Assert-Condition (@(Get-Receipts | Where-Object { -not $_.authorized }).Count -eq 1) 'The wrong bearer did not receive the independent unauthorized control.'
    foreach ($taskName in @('corrupt-token', 'plaintext-token', 'uppercase-token', 'newline-token', 'invalid-utf8-token', 'empty-token', 'oversize-token')) {
        $taskBadPath = Join-Path $taskRoot $taskName; $taskBefore = Get-Digest $taskBadPath; $taskCount = @(Get-Receipts).Count
        Assert-Condition ((Run-Sender @{ TokenFile = $taskBadPath }) -ne 0 -and @(Get-Receipts).Count -eq $taskCount -and
            (Get-Digest $taskBadPath) -eq $taskBefore) 'An invalid token was transmitted, accepted or rewritten.'
    }
    Write-Output 'PASS wrong, corrupt, plaintext, malformed, empty and oversized token denial without disclosure or rotation'
    foreach ($taskOverride in @(@{ Endpoint = 'http://example.invalid:17865/v1/editor/observations' }, @{ Endpoint = $taskEndpoint + '?x=1' },
        @{ Endpoint = $taskEndpoint.Replace('http:', 'https:') }, @{ Path = $taskTokenFile }, @{ ProjectRoot = (Join-Path $taskRoot 'missing') })) {
        Assert-Condition ((Run-Sender $taskOverride) -ne 0 -and (Get-AuthorizedCount) -eq $taskBase) 'An unsafe endpoint or outside-root file was transmitted.'
    }
    [IO.File]::WriteAllBytes($taskFile, [byte[]]@(255, 254))
    Assert-Condition ((Run-Sender) -ne 0) 'Invalid UTF-8 source was transmitted.'
    [IO.File]::WriteAllBytes($taskFile, (New-Object byte[] 16385))
    Assert-Condition ((Run-Sender) -ne 0 -and (Get-AuthorizedCount) -eq $taskBase) 'Oversized source was transmitted.'
    Set-SavedText "int saved = 1;`n"
    Write-Output 'PASS endpoint, root, source UTF-8 and byte bounds'
    $taskWatcher = Start-Script $taskWatcherScript @{ DebounceMilliseconds = '600' }
    Start-Sleep -Milliseconds 2500
    Assert-Condition (-not $taskWatcher.Process.HasExited -and (Get-AuthorizedCount) -eq $taskBase) 'The watcher failed or emitted an initial event.'
    [IO.File]::WriteAllText($taskOther, 'other changed', $taskUtf8); Start-Sleep -Milliseconds 900
    Assert-Condition ((Get-AuthorizedCount) -eq $taskBase) 'The watcher observed another file.'
    Set-SavedText "int saved = 2;`n"; Set-SavedText "int saved = 3;`n"; Wait-AuthorizedCount ($taskBase + 1)
    Assert-Condition (@(Get-Receipts | Where-Object { $_.authorized })[-1].body.revision -eq (Get-Digest $taskFile)) 'The watcher sent an outdated saved revision.'
    Set-SavedText "int saved = 3;`n"; Start-Sleep -Milliseconds 1400
    Assert-Condition ((Get-AuthorizedCount) -eq ($taskBase + 1)) 'The watcher duplicated identical saved bytes.'
    Write-Output 'PASS exact-file watcher, no initial event, debounce and duplicate suppression'
    [IO.File]::WriteAllText($taskModePath, 'busy_once'); Set-SavedText "int saved = 4;`n"; Wait-AuthorizedCount ($taskBase + 2)
    Set-SavedText "int saved = 5;`n"; Wait-AuthorizedCount ($taskBase + 3); Start-Sleep -Milliseconds 1200
    Assert-Condition ((Get-AuthorizedCount) -eq ($taskBase + 3) -and @(Get-Receipts | Where-Object { $_.authorized })[-1].body.revision -eq (Get-Digest $taskFile)) 'Busy recovery replayed or duplicated stale bytes.'
    [IO.File]::WriteAllText($taskModePath, 'busy_always'); Set-SavedText "int saved = 6;`n"; Wait-AuthorizedCount ($taskBase + 7) 12
    Start-Sleep -Milliseconds 1500
    Assert-Condition ((Get-AuthorizedCount) -eq ($taskBase + 7)) 'Busy retries exceeded their bound.'
    [IO.File]::WriteAllText($taskModePath, 'reject'); Set-SavedText "int saved = 7;`n"; Wait-AuthorizedCount ($taskBase + 8); Start-Sleep -Milliseconds 1500
    Assert-Condition ((Get-AuthorizedCount) -eq ($taskBase + 8)) 'A policy refusal was retried.'
    Write-Output 'PASS newest-revision busy recovery, bounded retry backoff and no policy replay'
    [IO.File]::WriteAllText($taskModePath, 'accept')
    $taskReplacement = Join-Path $taskProject 'atomic-save.tmp'; [IO.File]::WriteAllText($taskReplacement, "int saved = 8;`n", $taskUtf8)
    $taskBackup = Join-Path $taskProject 'atomic-backup.cpp'
    [IO.File]::Replace($taskReplacement, $taskFile, $taskBackup); Wait-AuthorizedCount ($taskBase + 9)
    Assert-Condition (@(Get-Receipts | Where-Object { $_.authorized })[-1].body.revision -eq (Get-Digest $taskFile)) 'The watcher missed atomic saved-file replacement.'
    Assert-Condition ((Get-Digest $taskTokenFile) -eq $taskProtectedDigest) 'A script rewrote the protected token.'
    Write-Output 'PASS atomic replacement and unchanged protected token'
}
finally {
    $taskCleanupIssue = $null
    if ($null -ne $taskWatcher) {
        if (-not $taskWatcher.Process.HasExited) { $taskWatcher.Process.Kill(); $null = $taskWatcher.Process.WaitForExit(3000) }
        $taskWatcherOutput = $taskWatcher.Output.GetAwaiter().GetResult() + $taskWatcher.Error.GetAwaiter().GetResult()
        $taskWatcher.Process.Dispose()
        foreach ($taskSecret in $taskSecrets) { if ($taskWatcherOutput.Contains($taskSecret)) { $taskCleanupIssue = 'The watcher disclosed a bearer token.' } }
    }
    if ($null -ne $taskServer) { Stop-Job -Job $taskServer -ErrorAction SilentlyContinue; Remove-Job -Job $taskServer -Force -ErrorAction SilentlyContinue }
    if ($null -ne $taskNativeBytes -and [IO.File]::Exists($taskNativePath)) {
        $taskCurrentNative = [IO.File]::ReadAllBytes($taskNativePath)
        if ([Convert]::ToBase64String($taskCurrentNative) -cne [Convert]::ToBase64String($taskNativeBytes)) { $taskCleanupIssue = 'The native token bytes changed during the read-only control.' }
    }
    $taskResolved = [IO.Path]::GetFullPath($taskRoot)
    if ($taskResolved -ne (Join-Path $taskTempParent $taskLeaf) -or (Split-Path -Parent $taskResolved) -ne $taskTempParent.TrimEnd('\') -or
        (((Get-Item -LiteralPath $taskResolved).Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) { throw 'Refusing fixture cleanup outside its exact temporary directory.' }
    Remove-Item -LiteralPath $taskResolved -Recurse -Force
    if ($taskCleanupIssue) { throw $taskCleanupIssue }
}
