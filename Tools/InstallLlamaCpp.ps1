param(
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$installRoot = Join-Path $repoRoot 'ThirdParty\llama.cpp'
$version = 'b10453'
$llamaUrl = "https://github.com/ggml-org/llama.cpp/releases/download/$version/llama-$version-bin-win-cuda-12.4-x64.zip"
$llamaSha256 = '84b863f70a8b4c2873e93385d0b208f24776ecd1b946a2cb6d5cda863d143c3d'
$cudaUrl = "https://github.com/ggml-org/llama.cpp/releases/download/$version/cudart-llama-bin-win-cuda-12.4-x64.zip"
$cudaSha256 = '8c79a9b226de4b3cacfd1f83d24f962d0773be79f1e7b75c6af4ded7e32ae1d6'
$llamaArchive = Join-Path $env:TEMP "revia-llama-$version.zip"
$cudaArchive = Join-Path $env:TEMP "revia-llama-cudart-$version.zip"
$extractRoot = Join-Path $env:TEMP ("revia-llama-extract-" + [Guid]::NewGuid().ToString('N'))

function Get-VerifiedDownload {
    param(
        [Parameter(Mandatory = $true)][string]$Uri,
        [Parameter(Mandatory = $true)][string]$Destination,
        [Parameter(Mandatory = $true)][string]$Sha256
    )
    if ((Test-Path -LiteralPath $Destination -PathType Leaf) -and -not $Force) {
        if ((Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash.ToLowerInvariant() -eq $Sha256) {
            return
        }
    }
    Invoke-WebRequest -Uri $Uri -OutFile $Destination
    $actual = (Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $Sha256) {
        throw "Checksum mismatch for $Destination. Expected $Sha256 but received $actual."
    }
}

if ((Test-Path -LiteralPath (Join-Path $installRoot 'llama-server.exe')) -and -not $Force) {
    Write-Host "llama.cpp is already installed: $installRoot\llama-server.exe"
    exit 0
}

Get-VerifiedDownload -Uri $llamaUrl -Destination $llamaArchive -Sha256 $llamaSha256
Get-VerifiedDownload -Uri $cudaUrl -Destination $cudaArchive -Sha256 $cudaSha256
New-Item -ItemType Directory -Path $extractRoot -Force | Out-Null
$llamaExtract = Join-Path $extractRoot 'llama'
$cudaExtract = Join-Path $extractRoot 'cuda'
Expand-Archive -LiteralPath $llamaArchive -DestinationPath $llamaExtract -Force
Expand-Archive -LiteralPath $cudaArchive -DestinationPath $cudaExtract -Force

$server = Get-ChildItem -LiteralPath $llamaExtract -Filter 'llama-server.exe' -File -Recurse |
    Select-Object -First 1
if ($null -eq $server) {
    throw 'The verified llama.cpp package did not contain llama-server.exe.'
}

New-Item -ItemType Directory -Path $installRoot -Force | Out-Null
Get-ChildItem -LiteralPath $server.Directory.FullName -File | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $installRoot -Force
}
Get-ChildItem -LiteralPath $cudaExtract -Filter '*.dll' -File -Recurse | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $installRoot -Force
}

$resolvedExtract = [IO.Path]::GetFullPath($extractRoot)
$resolvedTemp = [IO.Path]::GetFullPath($env:TEMP).TrimEnd('\') + '\'
if (-not $resolvedExtract.StartsWith($resolvedTemp, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to remove an extraction path outside the temporary directory: $resolvedExtract"
}
Remove-Item -LiteralPath $resolvedExtract -Recurse -Force
Write-Host "llama.cpp $version ready: $installRoot\llama-server.exe"
