param(
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$destination = Join-Path $repoRoot 'Models\Qwen3-VL-8B-Instruct-Unredacted-MAX.mmproj-q8_0.gguf'
$uri = 'https://huggingface.co/prithivMLmods/Qwen3-VL-8B-Instruct-Unredacted-MAX-GGUF/resolve/main/Qwen3-VL-8B-Instruct-Unredacted-MAX.mmproj-q8_0.gguf'
$sha256 = '20e21104e70d6363dca590bc628e7e4d55fc8e0c1152ea49b7607d2ab4a30daf'

if ((Test-Path -LiteralPath $destination -PathType Leaf) -and -not $Force) {
    $existing = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($existing -eq $sha256) {
        Write-Host "Vision projector is already verified: $destination"
        exit 0
    }
}

New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
Invoke-WebRequest -Uri $uri -OutFile $destination
$actual = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actual -ne $sha256) {
    throw "Vision projector checksum mismatch. Expected $sha256 but received $actual."
}
Write-Host "Vision projector ready: $destination"
