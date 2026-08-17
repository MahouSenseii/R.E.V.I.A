param(
    [switch]$Force,
    [ValidateSet('auto', 'cpu', 'vulkan', 'cuda')]
    [string]$Accelerator = 'auto'
)

# Installs whisper.cpp plus the small.en model. The cuBLAS package is 640 MB and
# will not run without an NVIDIA GPU, so anything else gets the 20 MB OpenBLAS
# CPU build instead. whisper.cpp v1.9.2 publishes no Vulkan build for Windows.
#
# This script also aligns speechRecognition.useGpu in Config/settings.json with
# the runtime it installed, because passing -ng is what makes the CPU build work
# and the C++ side already honours that flag.

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'ReviaAcceleration.ps1')

$installRoot = Join-Path $repoRoot 'ThirdParty\whisper'
$modelPath = Join-Path $repoRoot 'Models\ggml-small.en.bin'
$settingsPath = Join-Path $repoRoot 'Config\settings.json'
$modelUrl = 'https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small.en.bin'
$modelSha256 = 'c6138d6d58ecc8322097e0f987c32f1be8bb0a18532a3f88f734d1bbf9c41e5d'

$resolved = Resolve-ReviaAccelerator -Requested $Accelerator
$package = Get-ReviaWhisperPackage -Accelerator $resolved

$archivePath = Join-Path $env:TEMP "revia-$($package.Name)"
$extractPath = Join-Path $env:TEMP ('revia-whisper-extract-' + [Guid]::NewGuid().ToString('N'))

New-Item -ItemType Directory -Path (Split-Path -Parent $modelPath) -Force | Out-Null
Get-ReviaVerifiedDownload -Uri $package.Uri -Destination $archivePath -Sha256 $package.Sha -Force:$Force
Get-ReviaVerifiedDownload -Uri $modelUrl -Destination $modelPath -Sha256 $modelSha256 -Force:$Force

try {
    New-Item -ItemType Directory -Path $extractPath -Force | Out-Null
    Expand-Archive -LiteralPath $archivePath -DestinationPath $extractPath -Force

    $cli = Get-ChildItem -LiteralPath $extractPath -Filter 'whisper-cli.exe' -File -Recurse |
        Select-Object -First 1
    if ($null -eq $cli) {
        throw "The verified whisper.cpp package for $resolved did not contain whisper-cli.exe."
    }

    if (Test-Path -LiteralPath $installRoot) {
        Remove-Item -LiteralPath $installRoot -Recurse -Force
    }
    New-Item -ItemType Directory -Path $installRoot -Force | Out-Null
    Get-ChildItem -LiteralPath $cli.Directory.FullName -File | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $installRoot -Force
    }
}
finally {
    Remove-ReviaTempDirectory -Path $extractPath
}

# Keep configuration honest about what was installed. A CPU build launched
# without -ng still runs, but reporting GPU use it does not have makes the
# timing diagnostics misleading.
if (Test-Path -LiteralPath $settingsPath -PathType Leaf) {
    try {
        $settings = Get-Content -LiteralPath $settingsPath -Raw | ConvertFrom-Json
        if ($settings.speechRecognition.useGpu -ne $package.UseGpu) {
            $settings.speechRecognition.useGpu = $package.UseGpu
            $settings | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $settingsPath -Encoding UTF8
            Write-Host "Set speechRecognition.useGpu to $($package.UseGpu) in Config/settings.json."
            Write-Host 'Rebuild so the post-build copy updates build/debug/Config/.'
        }
    }
    catch {
        Write-Warning "Could not update speechRecognition.useGpu automatically: $($_.Exception.Message)"
        Write-Warning "Set it to $($package.UseGpu) manually in Config/settings.json."
    }
}

Write-Host "whisper.cpp v1.9.2 ($resolved) ready: $installRoot\whisper-cli.exe"
Write-Host "Whisper small.en model ready: $modelPath"
