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

Set-StrictMode -Version Latest

enum ReviaAccelerator {
    Cpu
    Vulkan
    Cuda
}

class ReviaAcceleratorProbe {
    [ReviaAccelerator] $Detected
    [string] $Reason

    ReviaAcceleratorProbe() {
        $this.Detected = [ReviaAccelerator]::Cpu
        $this.Reason = 'No hardware acceleration was detected.'
        $this.Probe()
    }

    hidden [void] Probe() {
        $adapters = @()
        try {
            $adapters = @(Get-CimInstance -ClassName Win32_VideoController -ErrorAction Stop)
        }
        catch {
            $this.Reason = 'The display adapter list could not be queried; assuming CPU only.'
            return
        }

        $names = @($adapters | ForEach-Object { $_.Name } | Where-Object { $_ })

        # NVIDIA first: the CUDA build is meaningfully faster than Vulkan on
        # NVIDIA hardware, which is why it stays the preferred path when present.
        $nvidia = @($names | Where-Object { $_ -match 'NVIDIA' })
        if ($nvidia.Count -gt 0) {
            $this.Detected = [ReviaAccelerator]::Cuda
            $this.Reason = "NVIDIA adapter detected: $($nvidia[0])."
            return
        }

        # vulkan-1.dll is installed by the graphics driver, so its presence is a
        # far better signal than adapter names for AMD and Intel parts.
        $vulkanLoader = Join-Path $env:SystemRoot 'System32\vulkan-1.dll'
        if (Test-Path -LiteralPath $vulkanLoader -PathType Leaf) {
            $discrete = @($names | Where-Object { $_ -match 'AMD|Radeon|Intel Arc' })
            $label = if ($discrete.Count -gt 0) { $discrete[0] } elseif ($names.Count -gt 0) { $names[0] } else { 'an unnamed adapter' }
            $this.Detected = [ReviaAccelerator]::Vulkan
            $this.Reason = "Vulkan runtime present for $label."
            return
        }

        if ($names.Count -gt 0) {
            $this.Reason = "No CUDA or Vulkan support found for $($names[0]); using the CPU build."
        }
    }
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
        $explicit = [ReviaAccelerator]$Requested
        Write-Host "Accelerator: $explicit (requested explicitly)."
        return $explicit
    }

    $probe = [ReviaAcceleratorProbe]::new()
    Write-Host "Accelerator: $($probe.Detected). $($probe.Reason)"
    return $probe.Detected
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
    # Invoke-WebRequest's progress bar costs more time than the transfer on
    # large archives when the host is not a real console.
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

function Get-ReviaLlamaPackages {
    <#
    .SYNOPSIS
        Returns the archives to install for a given accelerator.
    #>
    param(
        [Parameter(Mandatory = $true)][ReviaAccelerator]$Accelerator
    )

    $version = $script:ReviaLlamaVersion
    $base = "https://github.com/ggml-org/llama.cpp/releases/download/$version"

    switch ($Accelerator) {
        ([ReviaAccelerator]::Cuda) {
            return @(
                @{ Name = "llama-$version-bin-win-cuda-12.4-x64.zip"
                   Uri  = "$base/llama-$version-bin-win-cuda-12.4-x64.zip"
                   Sha  = '84b863f70a8b4c2873e93385d0b208f24776ecd1b946a2cb6d5cda863d143c3d' },
                @{ Name = 'cudart-llama-bin-win-cuda-12.4-x64.zip'
                   Uri  = "$base/cudart-llama-bin-win-cuda-12.4-x64.zip"
                   Sha  = '8c79a9b226de4b3cacfd1f83d24f962d0773be79f1e7b75c6af4ded7e32ae1d6' }
            )
        }
        ([ReviaAccelerator]::Vulkan) {
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

    throw "Unhandled accelerator: $Accelerator"
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
        [Parameter(Mandatory = $true)][ReviaAccelerator]$Accelerator
    )

    $base = 'https://github.com/ggml-org/whisper.cpp/releases/download/v1.9.2'

    switch ($Accelerator) {
        ([ReviaAccelerator]::Cuda) {
            return @{ Name  = 'whisper-cublas-12.4.0-bin-x64.zip'
                      Uri   = "$base/whisper-cublas-12.4.0-bin-x64.zip"
                      Sha   = '443110ddaad70d4290ab2e77179e31cf712035bbc4fad56bb4519a90c917b39c'
                      UseGpu = $true }
        }
        default {
            return @{ Name  = 'whisper-blas-bin-x64.zip'
                      Uri   = "$base/whisper-blas-bin-x64.zip"
                      Sha   = 'ffe5b47ca8e53a7677949f23a9c4641bbec4eee8a5714c3d14b67bb8d7b24a78'
                      UseGpu = $false }
        }
    }

    throw "Unhandled accelerator: $Accelerator"
}
