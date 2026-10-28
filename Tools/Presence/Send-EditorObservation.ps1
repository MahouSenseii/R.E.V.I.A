[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ProjectRoot,
    [Parameter(Mandatory = $true)][string]$Path,
    [Parameter(Mandatory = $true)][string]$TokenFile,
    [uri]$Endpoint = 'http://127.0.0.1:17865/v1/editor/observations'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Read-BoundedBytes([string]$TargetPath, [int]$MaximumBytes) {
    $taskStream = [IO.File]::Open($TargetPath, [IO.FileMode]::Open, [IO.FileAccess]::Read,
        ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    $taskBuffer = New-Object byte[] ($MaximumBytes + 1)
    $taskCount = 0
    try {
        while ($taskCount -lt $taskBuffer.Length) {
            $taskMore = $taskStream.Read($taskBuffer, $taskCount, $taskBuffer.Length - $taskCount)
            if ($taskMore -eq 0) { break }
            $taskCount += $taskMore
        }
    }
    finally { $taskStream.Dispose() }
    if ($taskCount -gt $MaximumBytes) { throw 'The saved file or token file exceeds its byte limit.' }
    $taskData = New-Object byte[] $taskCount
    [Array]::Copy($taskBuffer, $taskData, $taskCount)
    return ,$taskData
}

# This sender reports a saved file's identity. It never sends file text or invokes chat.
if (-not $Endpoint.IsAbsoluteUri -or $Endpoint.Scheme -ne 'http' -or
    $Endpoint.Host -notin @('127.0.0.1', 'localhost') -or $Endpoint.Port -lt 1024 -or
    $Endpoint.AbsolutePath -ne '/v1/editor/observations' -or $Endpoint.UserInfo -or $Endpoint.Query -or $Endpoint.Fragment) {
    throw 'The editor endpoint must be the plain loopback HTTP editor-observation route.'
}
$taskRoot = [IO.Path]::GetFullPath($ProjectRoot)
if ($taskRoot.Length -gt [IO.Path]::GetPathRoot($taskRoot).Length) {
    $taskRoot = $taskRoot.TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
}
$taskPath = [IO.Path]::GetFullPath($Path)
$taskPrefix = $taskRoot.TrimEnd([IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
if ($taskRoot.Length -gt 1024 -or $taskPath.Length -gt 1024 -or
    -not [IO.Directory]::Exists($taskRoot) -or -not [IO.File]::Exists($taskPath) -or
    -not $taskPath.StartsWith($taskPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Choose an existing saved file inside the explicit project root.'
}
$taskFile = Get-Item -LiteralPath $taskPath
if ($taskFile.Length -gt 16384) { throw 'Saved editor files are limited to 16 KiB by this sender.' }
$taskBytes = Read-BoundedBytes $taskPath 16384
$taskUtf8 = New-Object Text.UTF8Encoding($false, $true)
try { $null = $taskUtf8.GetString($taskBytes) } catch { throw 'The saved file must contain valid UTF-8.' }
$taskSha = [Security.Cryptography.SHA256]::Create()
try { $taskRevision = [BitConverter]::ToString($taskSha.ComputeHash($taskBytes)).Replace('-', '').ToLowerInvariant() }
finally { $taskSha.Dispose() }
$taskTokenPath = [IO.Path]::GetFullPath($TokenFile)
if (-not [IO.File]::Exists($taskTokenPath) -or (Get-Item -LiteralPath $taskTokenPath).Length -gt 4096) {
    throw 'Choose the existing local API token file explicitly.'
}
$taskStoredToken = Read-BoundedBytes $taskTokenPath 4096
$taskTokenBytes = $null
$taskToken = $null
try {
    if ([Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT) {
        # Core/localApiToken owns this file. Windows stores a CurrentUser DPAPI blob,
        # without optional entropy; an unreadable blob must never become plaintext.
        Add-Type -AssemblyName System.Security
        $taskTokenBytes = [Security.Cryptography.ProtectedData]::Unprotect($taskStoredToken, $null,
            [Security.Cryptography.DataProtectionScope]::CurrentUser)
    }
    else { $taskTokenBytes = $taskStoredToken }
    if ($taskTokenBytes.Length -ne 70) { throw 'Invalid token length.' }
    $taskToken = $taskUtf8.GetString($taskTokenBytes)
    if ($taskToken.Length -ne 70 -or $taskToken -cnotmatch '\Arevia-[0-9a-f]{64}\z') {
        throw 'Invalid token format.'
    }
}
catch {
    $taskToken = $null
    throw 'The local API token could not be read for this user. Use the existing token file from the running installation.'
}
finally {
    if ($null -ne $taskTokenBytes) { [Array]::Clear($taskTokenBytes, 0, $taskTokenBytes.Length) }
    [Array]::Clear($taskStoredToken, 0, $taskStoredToken.Length)
}
$taskBody = @{
    source = 'saved_file'
    projectRoot = $taskRoot
    path = $taskPath
    revision = $taskRevision
    occurredAtUnixMs = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
} | ConvertTo-Json -Compress
if ([Text.Encoding]::UTF8.GetByteCount($taskBody) -gt 8192) { throw 'The observation metadata exceeds 8 KiB.' }

Add-Type -AssemblyName System.Net.Http
$taskHandler = New-Object Net.Http.HttpClientHandler
$taskHandler.UseProxy = $false
$taskHandler.AllowAutoRedirect = $false
$taskClient = New-Object Net.Http.HttpClient($taskHandler)
$taskClient.Timeout = [TimeSpan]::FromSeconds(5)
$taskContent = New-Object Net.Http.StringContent($taskBody, [Text.Encoding]::UTF8, 'application/json')
$taskRequest = New-Object Net.Http.HttpRequestMessage([Net.Http.HttpMethod]::Post, $Endpoint)
$taskRequest.Content = $taskContent
try {
    $taskClient.DefaultRequestHeaders.Authorization = New-Object Net.Http.Headers.AuthenticationHeaderValue('Bearer', $taskToken)
    try { $taskResponse = $taskClient.SendAsync($taskRequest, [Net.Http.HttpCompletionOption]::ResponseHeadersRead).GetAwaiter().GetResult() }
    catch { throw 'The local editor observation request failed.' }
    try {
        if ([int]$taskResponse.StatusCode -notin @(202, 409)) {
            throw ('The local editor observation was refused (HTTP {0}).' -f [int]$taskResponse.StatusCode)
        }
        $taskResponseStream = $taskResponse.Content.ReadAsStreamAsync().GetAwaiter().GetResult()
        $taskResponseBytes = New-Object byte[] 4097
        $taskRead = 0
        $taskReadTimeout = New-Object Threading.CancellationTokenSource
        $taskReadTimeout.CancelAfter(5000)
        try {
            while ($taskRead -lt $taskResponseBytes.Length) {
                $taskMore = $taskResponseStream.ReadAsync($taskResponseBytes, $taskRead, $taskResponseBytes.Length - $taskRead, $taskReadTimeout.Token).GetAwaiter().GetResult()
                if ($taskMore -eq 0) { break }
                $taskRead += $taskMore
            }
        }
        finally { $taskReadTimeout.Dispose(); $taskResponseStream.Dispose() }
        if ($taskRead -gt 4096) { throw 'The local editor response exceeded its metadata bound.' }
        $taskResponseText = $taskUtf8.GetString($taskResponseBytes, 0, $taskRead)
        $taskResult = $taskResponseText | ConvertFrom-Json
        if ($taskResult.accepted -isnot [bool] -or $taskResult.reason -isnot [string] -or $taskResult.eventId -isnot [string] -or
            $taskResult.retryable -isnot [bool] -or
            $taskResult.accepted -ne ([int]$taskResponse.StatusCode -eq 202)) { throw 'The local editor response was malformed.' }
        [pscustomobject]@{ accepted = $taskResult.accepted; reason = $taskResult.reason; eventId = $taskResult.eventId; revision = $taskRevision; retryable = $taskResult.retryable }
    }
    finally { $taskResponse.Dispose() }
}
finally {
    $taskToken = $null
    $taskRequest.Dispose()
    $taskContent.Dispose()
    $taskClient.Dispose()
    $taskHandler.Dispose()
}
