# Shared acceleration detection and verified-download support for the Revia
# runtime installers.
#
# Why this exists: InstallLlamaCpp.ps1 and InstallWhisper.ps1 both hardcoded
# CUDA 12.4 builds. That made a clean install cost ~1.25 GB and fail outright on
# a machine without an NVIDIA GPU. Both scripts now resolve a pinned asset for
# the accelerator actually present, and both share one download implementation
# instead of duplicating it.
#
# Every asset below is pinned by SHA-256 exactly as before. Digests are the
# values published on the upstream release pages for llama.cpp b10453 and
# whisper.cpp v1.9.2.
#
# Two deliberate constraints on how this file is written:
#
#   1. No `enum` or `class`. Windows PowerShell 5.1 resolves type literals when
#      it compiles the calling script, which happens before the dot-source that
#      defines them has run. `[ReviaAccelerator]::Cpu` in an installer would
#      therefore fail with "Unable to find type". Accelerators are plain strings
#      validated by ValidateSet, which costs a little type safety and removes
#      the failure mode entirely.
#
#   2. No Set-StrictMode. Dot-sourcing executes in the caller's scope, so a
#      strict mode set here silently applies to every installer that sources
#      this file. Under -Version Latest that turns a missing JSON property from
#      a null into a terminating error, which is not a decision this helper
#      should be making on the caller's behalf.

$script:ReviaAccelerators = @('cpu', 'vulkan', 'cuda')

function Get-ReviaAcceleratorProbe {
    <#
    .SYNOPSIS
        Inspects the display adapters and reports the best available accelerator.
    .OUTPUTS
        An object with Accelerator ('cpu', 'vulkan', or 'cuda') and Reason.
    #>

    $result = [pscustomobject]@{
        Accelerator = 'cpu'
        Reason      = 'No hardware acceleration was detected.'
    }

    $adapters = @()
    try {
        $adapters = @(Get-CimInstance -ClassName Win32_VideoController -ErrorAction Stop)
    }
    catch {
        $result.Reason = 'The display adapter list could not be queried; assuming CPU only.'
        return $result
    }

    $names = @($adapters | ForEach-Object { $_.Name } | Where-Object { $_ })

    # NVIDIA first: the CUDA build is meaningfully faster than Vulkan on NVIDIA
    # hardware, which is why it stays the preferred path when present.
    $nvidia = @($names | Where-Object { $_ -match 'NVIDIA' })
    if ($nvidia.Count -gt 0) {
        $result.Accelerator = 'cuda'
        $result.Reason = "NVIDIA adapter detected: $($nvidia[0])."
        return $result
    }

    # vulkan-1.dll is installed by the graphics driver, so its presence is a far
    # better signal than adapter names for AMD and Intel parts.
    $vulkanLoader = Join-Path $env:SystemRoot 'System32\vulkan-1.dll'
    if (Test-Path -LiteralPath $vulkanLoader -PathType Leaf) {
        $discrete = @($names | Where-Object { $_ -match 'AMD|Radeon|Intel Arc' })
        if ($discrete.Count -gt 0) {
            $label = $discrete[0]
        }
        elseif ($names.Count -gt 0) {
            $label = $names[0]
        }
        else {
            $label = 'an unnamed adapter'
        }
        $result.Accelerator = 'vulkan'
        $result.Reason = "Vulkan runtime present for $label."
        return $result
    }

    if ($names.Count -gt 0) {
        $result.Reason = "No CUDA or Vulkan support found for $($names[0]); using the CPU build."
    }
    return $result
}

function Resolve-ReviaAccelerator {
    <#
    .SYNOPSIS
        Resolves the requested accelerator, expanding 'auto' by probing hardware.
    #>
    param(
        [ValidateSet('auto', 'cpu', 'vulkan', 'cuda')]
        [string]$Requested = 'auto'
    )

    if ($Requested -ne 'auto') {
        Write-Host "Accelerator: $Requested (requested explicitly)."
        return $Requested
    }

    $probe = Get-ReviaAcceleratorProbe
    Write-Host "Accelerator: $($probe.Accelerator). $($probe.Reason)"
    return $probe.Accelerator
}

function Get-ReviaVerifiedDownload {
    <#
    .SYNOPSIS
        Downloads a file and refuses to proceed unless it matches a pinned digest.
    #>
    param(
        [Parameter(Mandatory = $true)][string]$Uri,
        [Parameter(Mandatory = $true)][string]$Destination,
        [Parameter(Mandatory = $true)][string]$Sha256,
        [switch]$Force
    )

    $expected = $Sha256.ToLowerInvariant()

    if ((Test-Path -LiteralPath $Destination -PathType Leaf) -and -not $Force) {
        $existing = (Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($existing -eq $expected) {
            Write-Host "Cached and verified: $(Split-Path -Leaf $Destination)"
            return
        }
    }

    Write-Host "Downloading $(Split-Path -Leaf $Uri)..."
    $previousProgress = $ProgressPreference
    # Invoke-WebRequest's progress bar costs more time than the transfer itself
    # on large archives when the host is not a real console.
    $ProgressPreference = 'SilentlyContinue'
    try {
        Invoke-WebRequest -Uri $Uri -OutFile $Destination
    }
    finally {
        $ProgressPreference = $previousProgress
    }

    $actual = (Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $expected) {
        throw "Checksum mismatch for $Destination. Expected $expected but received $actual."
    }
}

function Save-ReviaJsonFile {
    <#
    .SYNOPSIS
        Writes an object as JSON in UTF-8 without a byte-order mark.
    .NOTES
        Set-Content -Encoding UTF8 always emits a BOM on Windows PowerShell 5.1.
        Config/settings.json is tracked in Git and read by nlohmann::json. The
        parser tolerates a BOM, but silently adding one to a tracked file on
        every install produces confusing diffs, so write the bytes explicitly.
    #>
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)]$Value,
        [int]$Depth = 12
    )

    $json = $Value | ConvertTo-Json -Depth $Depth
    $utf8NoBom = [System.Text.UTF8Encoding]::new($false)
    [System.IO.File]::WriteAllText($Path, $json + [Environment]::NewLine, $utf8NoBom)
}

function Remove-ReviaTempDirectory {
    <#
    .SYNOPSIS
        Deletes a directory only after proving it sits inside %TEMP%.
    #>
    param(
        [Parameter(Mandatory = $true)][string]$Path
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        return
    }

    $resolved = [IO.Path]::GetFullPath($Path)
    $tempRoot = [IO.Path]::GetFullPath($env:TEMP).TrimEnd('\') + '\'
    if (-not $resolved.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove a path outside the temporary directory: $resolved"
    }

    Remove-Item -LiteralPath $resolved -Recurse -Force
}

# --- Pinned release assets -------------------------------------------------

$script:ReviaLlamaVersion = 'b10453'

function Get-ReviaLlamaVersion {
    return $script:ReviaLlamaVersion
}

function Get-ReviaLlamaPackages {
    <#
    .SYNOPSIS
        Returns the archives to install for a given accelerator.
    #>
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet('cpu', 'vulkan', 'cuda')]
        [string]$Accelerator
    )

    $version = $script:ReviaLlamaVersion
    $base = "https://github.com/ggml-org/llama.cpp/releases/download/$version"

    switch ($Accelerator) {
        'cuda' {
            return @(
                @{ Name = "llama-$version-bin-win-cuda-12.4-x64.zip"
                   Uri  = "$base/llama-$version-bin-win-cuda-12.4-x64.zip"
                   Sha  = '84b863f70a8b4c2873e93385d0b208f24776ecd1b946a2cb6d5cda863d143c3d' },
                @{ Name = 'cudart-llama-bin-win-cuda-12.4-x64.zip'
                   Uri  = "$base/cudart-llama-bin-win-cuda-12.4-x64.zip"
                   Sha  = '8c79a9b226de4b3cacfd1f83d24f962d0773be79f1e7b75c6af4ded7e32ae1d6' }
            )
        }
        'vulkan' {
            return @(
                @{ Name = "llama-$version-bin-win-vulkan-x64.zip"
                   Uri  = "$base/llama-$version-bin-win-vulkan-x64.zip"
                   Sha  = '123001c3e3918f29420f622431b06dfc5e09ef4d6aff366860d3fd5b9f3418d8' }
            )
        }
        default {
            return @(
                @{ Name = "llama-$version-bin-win-cpu-x64.zip"
                   Uri  = "$base/llama-$version-bin-win-cpu-x64.zip"
                   Sha  = '70c07211d0027305f0be09cd755d79641ebb0bb646590ff3d498c66b22df29b0' }
            )
        }
    }
}

function Get-ReviaWhisperPackage {
    <#
    .SYNOPSIS
        Returns the whisper.cpp archive to install for a given accelerator.
    .NOTES
        whisper.cpp v1.9.2 publishes no Vulkan build for Windows, so Vulkan
        machines get the OpenBLAS CPU build, which is still far smaller and
        faster than the 640 MB cuBLAS package they cannot use.
    #>
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet('cpu', 'vulkan', 'cuda')]
        [string]$Accelerator
    )

    $base = 'https://github.com/ggml-org/whisper.cpp/releases/download/v1.9.2'

    if ($Accelerator -eq 'cuda') {
        return @{ Name   = 'whisper-cublas-12.4.0-bin-x64.zip'
                  Uri    = "$base/whisper-cublas-12.4.0-bin-x64.zip"
                  Sha    = '443110ddaad70d4290ab2e77179e31cf712035bbc4fad56bb4519a90c917b39c'
                  UseGpu = $true }
    }

    return @{ Name   = 'whisper-blas-bin-x64.zip'
              Uri    = "$base/whisper-blas-bin-x64.zip"
              Sha    = 'ffe5b47ca8e53a7677949f23a9c4641bbec4eee8a5714c3d14b67bb8d7b24a78'
              UseGpu = $false }
}
