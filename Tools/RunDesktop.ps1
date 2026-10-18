param(
    [switch]$Release,
    [string]$RuntimeRoot
)

$ErrorActionPreference = 'Stop'
$reviaRoot = Split-Path -Parent $PSScriptRoot
$reviaRuntimeRoot = $reviaRoot
if ($PSBoundParameters.ContainsKey('RuntimeRoot')) {
    if ([string]::IsNullOrWhiteSpace($RuntimeRoot) -or
        $RuntimeRoot -notmatch '^(?:[A-Za-z]:[\\/]|\\\\[^\\/]+[\\/][^\\/]+(?:[\\/]|$))') {
        throw 'RuntimeRoot must be an absolute existing filesystem directory containing Config.'
    }
    if (-not (Test-Path -LiteralPath $RuntimeRoot -PathType Container)) {
        throw 'RuntimeRoot must be an absolute existing filesystem directory containing Config.'
    }
    $reviaRuntimeRoot = (Resolve-Path -LiteralPath $RuntimeRoot).ProviderPath
}
if (-not (Test-Path -LiteralPath (Join-Path $reviaRuntimeRoot 'Config') -PathType Container)) {
    throw 'RuntimeRoot must contain its existing Config directory.'
}
$reviaPreset = if ($Release) { 'release' } else { 'debug' }
$reviaExecutable = Join-Path $reviaRoot "build\$reviaPreset\ReviaDesktop.exe"
if (-not (Test-Path -LiteralPath $reviaExecutable -PathType Leaf)) {
    throw "Build Revia first with Tools\Build.ps1. Missing executable: $reviaExecutable"
}

# The explicit root keeps the selected companion stores and VM enrollment together.
Push-Location $reviaRuntimeRoot
try {
    & $reviaExecutable --runtime-root $reviaRuntimeRoot
}
finally {
    Pop-Location
}
