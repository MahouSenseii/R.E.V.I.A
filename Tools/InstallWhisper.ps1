param(
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$installRoot = Join-Path $repoRoot 'ThirdParty\whisper'
$modelPath = Join-Path $repoRoot 'Models\ggml-small.en.bin'
$archivePath = Join-Path $env:TEMP 'revia-whisper-cublas-12.4.0-v1.9.2.zip'
$extractPath = Join-Path $env:TEMP 'revia-whisper-v1.9.2-extract'
$archiveUrl = 'https://github.com/ggml-org/whisper.cpp/releases/download/v1.9.2/whisper-cublas-12.4.0-bin-x64.zip'
$archiveSha256 = '443110ddaad70d4290ab2e77179e31cf712035bbc4fad56bb4519a90c917b39c'
$modelUrl = 'https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small.en.bin'
$modelSha256 = 'c6138d6d58ecc8322097e0f987c32f1be8bb0a18532a3f88f734d1bbf9c41e5d'

function Get-VerifiedDownload {
    param(
        [Parameter(Mandatory = $true)][string]$Uri,
        [Parameter(Mandatory = $true)][string]$Destination,
        [Parameter(Mandatory = $true)][string]$Sha256
    )

    if ((Test-Path -LiteralPath $Destination -PathType Leaf) -and -not $Force) {
        $existing = (Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($existing -eq $Sha256) {
            return
        }
    }

    Invoke-WebRequest -Uri $Uri -OutFile $Destination
    $actual = (Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $Sha256) {
        throw "Checksum mismatch for $Destination. Expected $Sha256 but received $actual."
    }
}

New-Item -ItemType Directory -Path (Split-Path -Parent $modelPath) -Force | Out-Null
Get-VerifiedDownload -Uri $archiveUrl -Destination $archivePath -Sha256 $archiveSha256
Get-VerifiedDownload -Uri $modelUrl -Destination $modelPath -Sha256 $modelSha256

if (Test-Path -LiteralPath $extractPath) {
    Remove-Item -LiteralPath $extractPath -Recurse -Force
}
New-Item -ItemType Directory -Path $extractPath -Force | Out-Null
Expand-Archive -LiteralPath $archivePath -DestinationPath $extractPath -Force

$cli = Get-ChildItem -LiteralPath $extractPath -Filter 'whisper-cli.exe' -File -Recurse |
    Select-Object -First 1
if ($null -eq $cli) {
    throw 'The verified whisper.cpp package did not contain whisper-cli.exe.'
}

if (Test-Path -LiteralPath $installRoot) {
    Remove-Item -LiteralPath $installRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $installRoot -Force | Out-Null
Get-ChildItem -LiteralPath $cli.Directory.FullName -File | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $installRoot -Force
}

Remove-Item -LiteralPath $extractPath -Recurse -Force
Write-Host "whisper.cpp v1.9.2 ready: $installRoot\whisper-cli.exe"
Write-Host "Whisper small.en model ready: $modelPath"
