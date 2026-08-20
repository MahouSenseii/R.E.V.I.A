param(
    [switch]$Force,
    [ValidateSet('auto', 'cpu', 'vulkan', 'cuda')]
    [string]$Accelerator = 'auto',
    [ValidateSet('auto', '12.4', '13.3')]
    [string]$CudaRuntime = 'auto'
)

# Installs the llama.cpp Windows runtime matching the accelerator on this
# machine rather than always fetching the 612 MB CUDA pair. Sizes for b10453:
#   cuda   239 MB + 373 MB cudart   NVIDIA only, fastest on NVIDIA
#   vulkan  33 MB                   NVIDIA, AMD, and Intel
#   cpu     18 MB                   runs anywhere
#
# Pass -Accelerator explicitly to override detection, for example when building
# a redistributable package for a machine other than this one.

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'ReviaAcceleration.ps1')

$installRoot = Join-Path $repoRoot 'ThirdParty\llama.cpp'
$serverPath = Join-Path $installRoot 'llama-server.exe'
$stampPath = Join-Path $installRoot 'revia-backend.json'

$resolved = Resolve-ReviaAccelerator -Requested $Accelerator
$probe = Get-ReviaAcceleratorProbe
$resolvedCudaRuntime = if ($resolved -ne 'cuda') {
    $null
}
elseif ($CudaRuntime -ne 'auto') {
    $CudaRuntime
}
elseif ($probe.Accelerator -eq 'cuda' -and $probe.CudaRuntime) {
    $probe.CudaRuntime
}
else {
    '12.4'
}

# Reinstall when the accelerator changed even if a server binary is present,
# otherwise a machine that gained or lost a GPU keeps the wrong runtime.
if ((Test-Path -LiteralPath $serverPath -PathType Leaf) -and -not $Force) {
    $installedAccelerator = $null
    $installedCudaRuntime = $null
    if (Test-Path -LiteralPath $stampPath -PathType Leaf) {
        try {
            $stamp = Get-Content -LiteralPath $stampPath -Raw | ConvertFrom-Json
            $installedAccelerator = $stamp.accelerator
            $installedCudaRuntime = $stamp.cudaRuntime
        }
        catch {
            $installedAccelerator = $null
        }
    }

    if ($installedAccelerator -eq $resolved -and
        ($resolved -ne 'cuda' -or $installedCudaRuntime -eq $resolvedCudaRuntime)) {
        $runtimeLabel = if ($resolvedCudaRuntime) { ", CUDA $resolvedCudaRuntime" } else { '' }
        Write-Host "llama.cpp ($installedAccelerator$runtimeLabel) is already installed: $serverPath"
        exit 0
    }

    Write-Host "Replacing the installed llama.cpp runtime ($installedAccelerator) with $resolved."
}

$packages = if ($resolved -eq 'cuda') {
    Get-ReviaLlamaPackages -Accelerator $resolved -CudaRuntime $resolvedCudaRuntime
}
else {
    Get-ReviaLlamaPackages -Accelerator $resolved
}
$extractRoot = Join-Path $env:TEMP ('revia-llama-extract-' + [Guid]::NewGuid().ToString('N'))

try {
    New-Item -ItemType Directory -Path $extractRoot -Force | Out-Null
    $payloadRoot = Join-Path $extractRoot 'payload'
    New-Item -ItemType Directory -Path $payloadRoot -Force | Out-Null

    foreach ($package in $packages) {
        $archivePath = Join-Path $env:TEMP "revia-$($package.Name)"
        Get-ReviaVerifiedDownload -Uri $package.Uri -Destination $archivePath -Sha256 $package.Sha -Force:$Force
        Expand-Archive -LiteralPath $archivePath -DestinationPath $payloadRoot -Force
    }

    $server = Get-ChildItem -LiteralPath $payloadRoot -Filter 'llama-server.exe' -File -Recurse |
        Select-Object -First 1
    if ($null -eq $server) {
        throw "The verified llama.cpp package for $resolved did not contain llama-server.exe."
    }

    New-Item -ItemType Directory -Path $installRoot -Force | Out-Null

    # Copy the directory holding the server first, then any loose DLLs shipped
    # elsewhere in the payload, which is how the CUDA runtime archive is laid out.
    Get-ChildItem -LiteralPath $server.Directory.FullName -File | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $installRoot -Force
    }
    Get-ChildItem -LiteralPath $payloadRoot -Filter '*.dll' -File -Recurse | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $installRoot -Force
    }

    Save-ReviaJsonFile -Path $stampPath -Value ([ordered]@{
        accelerator = $resolved
        cudaRuntime = $resolvedCudaRuntime
        version     = Get-ReviaLlamaVersion
        installedAt = (Get-Date).ToString('o')
    })
}
finally {
    Remove-ReviaTempDirectory -Path $extractRoot
}

$runtimeLabel = if ($resolvedCudaRuntime) { ", CUDA $resolvedCudaRuntime" } else { '' }
Write-Host "llama.cpp $(Get-ReviaLlamaVersion) ($resolved$runtimeLabel) ready: $serverPath"
if ($resolved -eq 'cpu') {
    Write-Host 'Chat will run on the CPU. Expect slower generation and prefer a smaller quantised model.'
}
