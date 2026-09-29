param(
    # Where Revia runs from: the directory whose RuntimeData\Secrets will hold the key.
    # The debug build's runtime root when it exists, otherwise the repository.
    [string]$RuntimeRoot,
    # The secret's name, matching intelligence.advisor.keyName.
    [string]$Name = 'advisor',
    # Also turn intelligence.advisor.enabled on in Config/settings.json.
    [switch]$Enable,
    # Remove the stored key instead of writing one.
    [switch]$Forget
)

# Stores the advisor's API key the only way Revia reads one: as a DPAPI blob bound to
# this Windows account and to the store's entropy, under RuntimeData\Secrets. The key
# is asked for as a hidden prompt so it never sits in a command line, a shell history
# or a log. Nothing about it goes into settings.json, which refuses a key outright.

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $RuntimeRoot) {
    $debugRoot = Join-Path $repoRoot 'build\debug'
    $RuntimeRoot = if (Test-Path -LiteralPath (Join-Path $debugRoot 'Config') -PathType Container) { $debugRoot } else { $repoRoot }
}
if ($Name -notmatch '^[A-Za-z0-9_-]{1,64}$') {
    throw "'$Name' is not a secret name (letters, digits, '_' and '-')."
}
$secretsRoot = Join-Path $RuntimeRoot 'RuntimeData\Secrets'
$blobPath = Join-Path $secretsRoot "$Name.dpapi"

if ($Forget) {
    if (Test-Path -LiteralPath $blobPath -PathType Leaf) {
        Remove-Item -LiteralPath $blobPath -Force
        Write-Host "Forgot the '$Name' key: $blobPath"
    } else {
        Write-Host "There was no '$Name' key at $blobPath."
    }
    return
}

Add-Type -AssemblyName System.Security
# The same entropy Core/secretStore.cpp binds its blobs to; the two must agree or the
# program cannot open what this wrote.
$entropy = [System.Text.Encoding]::UTF8.GetBytes('revia.secret-store.v1')

$secure = Read-Host -Prompt "Paste the $Name API key (hidden)" -AsSecureString
$pointer = [System.Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
try {
    $plain = [System.Runtime.InteropServices.Marshal]::PtrToStringBSTR($pointer)
} finally {
    [System.Runtime.InteropServices.Marshal]::ZeroFreeBSTR($pointer)
}
$plain = $plain.Trim()
if (-not $plain) {
    throw 'No key was entered.'
}
$bytes = [System.Text.Encoding]::UTF8.GetBytes($plain)
$blob = [System.Security.Cryptography.ProtectedData]::Protect($bytes, $entropy, 'CurrentUser')
[Array]::Clear($bytes, 0, $bytes.Length)
$plain = $null

New-Item -ItemType Directory -Force -Path $secretsRoot | Out-Null
[System.IO.File]::WriteAllBytes($blobPath, $blob)
Write-Host "Stored the '$Name' key for this Windows account: $blobPath"

if ($Enable) {
    $settingsPath = Join-Path $repoRoot 'Config\settings.json'
    if (Test-Path -LiteralPath $settingsPath -PathType Leaf) {
        . (Join-Path $PSScriptRoot 'ReviaAcceleration.ps1') # Save-ReviaJsonFile
        $settings = Get-Content -LiteralPath $settingsPath -Raw | ConvertFrom-Json
        if ($null -eq $settings.intelligence) {
            $settings | Add-Member -NotePropertyName 'intelligence' -NotePropertyValue ([pscustomobject]@{}) -Force
        }
        if ($null -eq $settings.intelligence.advisor) {
            $settings.intelligence | Add-Member -NotePropertyName 'advisor' -NotePropertyValue ([pscustomobject]@{}) -Force
        }
        $settings.intelligence.advisor | Add-Member -NotePropertyName 'enabled' -NotePropertyValue $true -Force
        $settings.intelligence.advisor | Add-Member -NotePropertyName 'keyName' -NotePropertyValue $Name -Force
        Save-ReviaJsonFile -Path $settingsPath -Value $settings
        Write-Host 'Config/settings.json: intelligence.advisor.enabled = true. She consults only when asked ("ask Claude ...", /consult) until intelligence.advisor.escalation is "auto".'
    } else {
        Write-Warning "Config/settings.json was not found; set intelligence.advisor.enabled to true yourself."
    }
}
