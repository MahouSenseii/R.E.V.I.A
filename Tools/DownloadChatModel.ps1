param(
    [switch]$Force,
    [ValidateSet('Q8_0', 'BF16')]
    [string]$Quantisation = 'Q8_0'
)

# Downloads the chat GGUF and aligns Config/settings.json with what it fetched.
#
# Config/settings.json previously referenced Q4_K_M, which the upstream
# repository does not publish. The projector installed by
# InstallVisionProjector.ps1 comes from this same repository, and the vision
# projector must match the base model, so the chat weights are pinned here
# rather than left to a manual download that can silently diverge.
#
#   Q8_0   8.1 GiB   fits a 12 GB card alongside the 717 MiB projector
#   BF16  15.3 GiB   needs more VRAM than any single card in use here
#
# Digests are the Hugging Face LFS object IDs, which are SHA-256 values. The
# projector's published OID matches the digest already pinned in
# InstallVisionProjector.ps1, which confirms the mapping.

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'ReviaAcceleration.ps1')   # Save-ReviaJsonFile
$settingsPath = Join-Path $repoRoot 'Config\settings.json'
$repository = 'prithivMLmods/Qwen3-VL-8B-Instruct-Unredacted-MAX-GGUF'

$variants = @{
    'Q8_0' = @{
        FileName = 'Qwen3-VL-8B-Instruct-Unredacted-MAX.Q8_0.gguf'
        Sha256   = '193bed355c93e7a5af6841128a25dd6f1d7c65a6ba1b171c5bd18c2fe09427ae'
        Bytes    = 8709519648
    }
    'BF16' = @{
        FileName = 'Qwen3-VL-8B-Instruct-Unredacted-MAX.BF16.gguf'
        Sha256   = '1ef2e9057c85d59d1c6123d3fe79d2662b69a6562cd872015e5b3714fc67bf3f'
        Bytes    = 16388045088
    }
}

$variant = $variants[$Quantisation]
$destination = Join-Path $repoRoot "Models\$($variant.FileName)"
$uri = "https://huggingface.co/$repository/resolve/main/$($variant.FileName)?download=true"

function Test-ExistingModel {
    param([string]$Path, [string]$Sha256)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return $false
    }
    Write-Host 'Verifying the existing model file (this takes a minute)...'
    $existing = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    return $existing -eq $Sha256
}

if ((Test-ExistingModel -Path $destination -Sha256 $variant.Sha256) -and -not $Force) {
    Write-Host "Chat model is already verified: $destination"
}
else {
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null

    $gigabytes = [math]::Round($variant.Bytes / 1GB, 2)
    Write-Host "Downloading $($variant.FileName) ($gigabytes GiB)..."
    Write-Host 'This is a large transfer. It can be interrupted and re-run safely.'

    # Invoke-WebRequest buffers the whole response in memory on Windows
    # PowerShell, which is not viable at this size. Stream to disk instead.
    $partial = "$destination.partial"
    $client = [System.Net.Http.HttpClient]::new()
    $client.Timeout = [TimeSpan]::FromHours(6)
    try {
        $response = $client.GetAsync($uri, [System.Net.Http.HttpCompletionOption]::ResponseHeadersRead).
            GetAwaiter().GetResult()
        if (-not $response.IsSuccessStatusCode) {
            throw "Download failed with HTTP $([int]$response.StatusCode) $($response.ReasonPhrase)."
        }

        $source = $response.Content.ReadAsStreamAsync().GetAwaiter().GetResult()
        $target = [System.IO.File]::Create($partial)
        try {
            $buffer = [byte[]]::new(1MB)
            $written = 0L
            $lastReport = -1
            while (($read = $source.Read($buffer, 0, $buffer.Length)) -gt 0) {
                $target.Write($buffer, 0, $read)
                $written += $read
                $percent = [int](($written / $variant.Bytes) * 100)
                if ($percent -ne $lastReport -and $percent % 5 -eq 0) {
                    Write-Host "  $percent%  ($([math]::Round($written / 1GB, 2)) GiB)"
                    $lastReport = $percent
                }
            }
        }
        finally {
            $target.Dispose()
            $source.Dispose()
        }
    }
    finally {
        $client.Dispose()
    }

    Write-Host 'Verifying the download...'
    $actual = (Get-FileHash -LiteralPath $partial -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $variant.Sha256) {
        Remove-Item -LiteralPath $partial -Force
        throw "Checksum mismatch. Expected $($variant.Sha256) but received $actual. The partial file was removed."
    }

    Move-Item -LiteralPath $partial -Destination $destination -Force
    Write-Host "Chat model ready: $destination"
}

# Keep configuration pointing at a file that actually exists. This is the bug
# that left the project with no usable chat backend.
$settings = Get-Content -LiteralPath $settingsPath -Raw | ConvertFrom-Json
$relativePath = "Models/$($variant.FileName)"
if ($settings.llm.modelPath -ne $relativePath -or $settings.llm.modelName -ne $variant.FileName) {
    $settings.llm.modelPath = $relativePath
    $settings.llm.modelName = $variant.FileName
    Save-ReviaJsonFile -Path $settingsPath -Value $settings
    Write-Host "Updated llm.modelPath and llm.modelName in Config/settings.json."
    Write-Host 'Rebuild so the post-build copy updates build/debug/Config/.'
}

Write-Host ''
Write-Host 'Remaining setup:'
Write-Host '    .\Tools\InstallVisionProjector.ps1   # required for vision, must match this model'
Write-Host '    .\Tools\InstallLlamaCpp.ps1'
Write-Host '    .\Tools\DownloadEmbeddingModel.ps1'
