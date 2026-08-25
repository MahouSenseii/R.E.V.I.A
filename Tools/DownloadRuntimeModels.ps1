param(
    [ValidateSet('Minimal', 'Standard', 'Full')]
    [string]$Profile = 'Standard',
    [switch]$IncludeBackgroundModel,
    [switch]$Force,
    [switch]$SkipConfigUpdate
)

# Installs the latency-first model stack selected for the target dual-GPU machine.
# Every URL is pinned to a repository commit and every completed file is checked against
# the upstream LFS SHA-256 before it can replace a runtime artifact. Existing legacy
# models are deliberately left alone.

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'ReviaAcceleration.ps1') # Save-ReviaJsonFile

$manifestPath = Join-Path $repoRoot 'Config\model_manifest.json'
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw "Model manifest was not found: $manifestPath"
}
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($IncludeBackgroundModel) {
    Write-Warning '-IncludeBackgroundModel is deprecated; selecting the Full profile.'
    $Profile = 'Full'
}
$profileIds = @($manifest.profiles.$Profile)
if ($profileIds.Count -eq 0) {
    throw "The $Profile model profile is empty or missing from $manifestPath."
}
$artifacts = @(
    foreach ($entry in $manifest.artifacts) {
        if ($profileIds -contains $entry.id) {
            @{
                Name = [string]$entry.file
                Repository = [string]$entry.repository
                Commit = [string]$entry.revision
                Sha256 = [string]$entry.sha256
                Bytes = [long]$entry.bytes
                Optional = $false
            }
        }
    }
)
if ($artifacts.Count -ne $profileIds.Count) {
    throw "The $Profile profile references an artifact missing from the model manifest."
}
Write-Host "Model profile: $Profile ($($artifacts.Count) verified artifacts)"

function Test-ReviaArtifact {
    param([string]$Path, [string]$Sha256)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return $false
    }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() -eq
        $Sha256.ToLowerInvariant()
}

function Move-ReviaVerifiedArtifact {
    param([string]$Source, [string]$Destination)
    for ($attempt = 0; $attempt -lt 20; $attempt++) {
        try {
            Move-Item -LiteralPath $Source -Destination $Destination -Force
            return
        } catch [System.IO.IOException] {
            if ($attempt -eq 19) {
                # Antivirus/OneDrive can retain a read handle after hashing. Copying is
                # still safe because the destination is verified by the caller before it
                # is accepted; the partial may be cleaned on a later run.
                Copy-Item -LiteralPath $Source -Destination $Destination -Force
                return
            }
            [GC]::Collect()
            [GC]::WaitForPendingFinalizers()
            Start-Sleep -Milliseconds 250
        }
    }
}

function Receive-ReviaArtifact {
    param([hashtable]$Artifact)

    $models = Join-Path $repoRoot 'Models'
    New-Item -ItemType Directory -Path $models -Force | Out-Null
    $destination = Join-Path $models $Artifact.Name
    if (Test-ReviaArtifact -Path $destination -Sha256 $Artifact.Sha256) {
        Write-Host "Verified: $($Artifact.Name)"
        return
    }
    if ((Test-Path -LiteralPath $destination -PathType Leaf) -and -not $Force) {
        throw "An unverified file already exists at $destination. Re-run with -Force to replace only that file."
    }

    $partial = "$destination.partial"
    $offset = if (Test-Path -LiteralPath $partial -PathType Leaf) {
        (Get-Item -LiteralPath $partial).Length
    } else { 0L }
    if ($offset -gt $Artifact.Bytes) {
        throw "The partial file for $($Artifact.Name) is larger than the pinned artifact. Remove only $partial and retry."
    }
    if ($offset -eq $Artifact.Bytes -and
        (Test-ReviaArtifact -Path $partial -Sha256 $Artifact.Sha256)) {
        Move-ReviaVerifiedArtifact -Source $partial -Destination $destination
        if (-not (Test-ReviaArtifact -Path $destination -Sha256 $Artifact.Sha256)) {
            throw "Promoting the verified partial failed for $($Artifact.Name)."
        }
        Write-Host "Ready: $destination"
        return
    }

    $uri = "https://huggingface.co/$($Artifact.Repository)/resolve/$($Artifact.Commit)/$($Artifact.Name)?download=true"
    $client = [System.Net.Http.HttpClient]::new()
    $client.Timeout = [TimeSpan]::FromHours(6)
    try {
        $request = [System.Net.Http.HttpRequestMessage]::new(
            [System.Net.Http.HttpMethod]::Get, $uri)
        if ($offset -gt 0) {
            $request.Headers.Range = [System.Net.Http.Headers.RangeHeaderValue]::new($offset, $null)
            Write-Host "Resuming $($Artifact.Name) at $([math]::Round($offset / 1GB, 2)) GiB..."
        } else {
            Write-Host "Downloading $($Artifact.Name) ($([math]::Round($Artifact.Bytes / 1GB, 2)) GiB)..."
        }
        $response = $client.SendAsync(
            $request, [System.Net.Http.HttpCompletionOption]::ResponseHeadersRead).
            GetAwaiter().GetResult()
        if (-not $response.IsSuccessStatusCode) {
            throw "Download failed with HTTP $([int]$response.StatusCode) $($response.ReasonPhrase)."
        }

        $append = $offset -gt 0 -and
            $response.StatusCode -eq [System.Net.HttpStatusCode]::PartialContent
        if (-not $append) { $offset = 0L }
        $mode = if ($append) {
            [System.IO.FileMode]::Append
        } else {
            [System.IO.FileMode]::Create
        }
        $source = $response.Content.ReadAsStreamAsync().GetAwaiter().GetResult()
        $target = [System.IO.File]::Open(
            $partial, $mode, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
        try {
            $buffer = [byte[]]::new(4MB)
            $written = $offset
            $lastReport = -1
            while (($read = $source.Read($buffer, 0, $buffer.Length)) -gt 0) {
                $target.Write($buffer, 0, $read)
                $written += $read
                $percent = [int](($written / $Artifact.Bytes) * 100)
                if ($percent -ne $lastReport -and $percent % 5 -eq 0) {
                    Write-Host "  $percent% ($([math]::Round($written / 1GB, 2)) GiB)"
                    $lastReport = $percent
                }
            }
        } finally {
            $target.Dispose()
            $source.Dispose()
            $response.Dispose()
            $request.Dispose()
        }
    } finally {
        $client.Dispose()
    }

    if ((Get-Item -LiteralPath $partial).Length -ne $Artifact.Bytes) {
        throw "Downloaded size did not match the pinned size for $($Artifact.Name). The partial file was retained for a safe retry."
    }
    Write-Host "Verifying $($Artifact.Name)..."
    if (-not (Test-ReviaArtifact -Path $partial -Sha256 $Artifact.Sha256)) {
        throw "SHA-256 verification failed for $($Artifact.Name). The partial file was retained for inspection."
    }
    Move-ReviaVerifiedArtifact -Source $partial -Destination $destination
    if (-not (Test-ReviaArtifact -Path $destination -Sha256 $Artifact.Sha256)) {
        throw "Promoting the verified download failed for $($Artifact.Name)."
    }
    Write-Host "Ready: $destination"
}

foreach ($artifact in $artifacts) {
    Receive-ReviaArtifact -Artifact $artifact
}

if (-not $SkipConfigUpdate) {
    $settingsPath = Join-Path $repoRoot 'Config\settings.json'
    $settings = Get-Content -LiteralPath $settingsPath -Raw | ConvertFrom-Json
    $settings.llm.modelName = 'Qwen3.5-4B-Q4_K_M.gguf'
    $settings.llm.modelPath = 'Models/Qwen3.5-4B-Q4_K_M.gguf'
    $settings.llm.multimodalProjectorPath = 'Models/mmproj-F16.gguf'
    $settings.llm.visionEnabled = $Profile -ne 'Minimal'
    if ($null -ne $settings.vision) {
        $settings.vision.enabled = $Profile -ne 'Minimal'
        $settings.vision.continuousAwareness = $Profile -ne 'Minimal'
    }
    if ($null -ne $settings.intelligence) {
        $settings.intelligence.fast.modelName = 'Qwen3.5-0.8B-Q4_K_M.gguf'
        $settings.intelligence.fast.modelPath = 'Models/Qwen3.5-0.8B-Q4_K_M.gguf'
        $settings.intelligence.expert.modelName =
            'Qwen3-VL-8B-Instruct-Unredacted-MAX.Q4_K_M.gguf'
        $settings.intelligence.expert.modelPath =
            'Models/Qwen3-VL-8B-Instruct-Unredacted-MAX.Q4_K_M.gguf'
        $settings.intelligence.expert.multimodalProjectorPath =
            'Models/Qwen3-VL-8B-Instruct-Unredacted-MAX.mmproj-q8_0.gguf'
        $settings.intelligence.fast.enabled = $Profile -in @('Minimal', 'Standard', 'Full')
        $settings.intelligence.expert.enabled = $Profile -eq 'Full'
    }
    $settings.embedding.modelPath = 'Models/nomic-embed-text-v1.5.Q4_K_M.gguf'
    $settings.speechRecognition.modelPath = 'Models/ggml-distil-small.en.bin'
    Save-ReviaJsonFile -Path $settingsPath -Value $settings
    Write-Host 'Updated Config/settings.json to the verified runtime stack.'
}

Write-Host 'Legacy model files were preserved.'
