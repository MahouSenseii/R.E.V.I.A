[CmdletBinding(SupportsShouldProcess)]
param(
    [Parameter(Mandatory)][string]$Ffmpeg,
    [Parameter(Mandatory)][string]$Ffprobe,
    [string]$RuntimeRoot = (Split-Path -Parent $PSScriptRoot),
    [string]$Receipt
)

$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath($RuntimeRoot)
$target = Join-Path $root 'ThirdParty/ffmpeg/bin'
$sources = @(
    @{ Source = (Get-Item -LiteralPath $Ffmpeg).FullName; Name = 'ffmpeg.exe' },
    @{ Source = (Get-Item -LiteralPath $Ffprobe).FullName; Name = 'ffprobe.exe' }
)
$ffmpegDirectory = Split-Path -Parent $sources[0].Source
foreach ($name in @('libopenh264.dll', 'LICENSE_ffmpeg.md', 'LICENSE_libopenh264', 'readme.txt'))
{
    $candidate = Join-Path $ffmpegDirectory $name
    if (Test-Path -LiteralPath $candidate -PathType Leaf)
    {
        $sources += @{ Source = (Get-Item -LiteralPath $candidate).FullName; Name = $name }
    }
}
$probePackage = Join-Path (Split-Path -Parent $sources[1].Source) 'package.json'
if (Test-Path -LiteralPath $probePackage -PathType Leaf)
{
    $sources += @{ Source = (Get-Item -LiteralPath $probePackage).FullName; Name = 'ffprobe-package.json' }
}
$files = foreach ($item in $sources)
{
    $destination = Join-Path $target $item.Name
    $hash = (Get-FileHash -LiteralPath $item.Source -Algorithm SHA256).Hash.ToLowerInvariant()
    if ((Test-Path -LiteralPath $destination) -and
        (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant() -ne $hash)
    {
        throw "A different installed file already exists: $destination. Review it before replacement."
    }
    [ordered]@{ source = $item.Source; target = $destination; sha256 = $hash; bytes = (Get-Item -LiteralPath $item.Source).Length }
}
$applied = $PSCmdlet.ShouldProcess($target, 'Copy the explicitly selected installed video decoder and accompanying files')
if ($applied)
{
    New-Item -ItemType Directory -Path $target -Force | Out-Null
    foreach ($file in $files)
    {
        Copy-Item -LiteralPath $file.source -Destination $file.target
        if ((Get-FileHash -LiteralPath $file.target -Algorithm SHA256).Hash.ToLowerInvariant() -ne $file.sha256)
        {
            throw "The copied decoder failed its content hash check: $($file.target)"
        }
    }
}
$result = [ordered]@{ applied = $applied; runtimeRoot = $root; atUtc = (Get-Date).ToUniversalTime().ToString('o'); files = @($files) }
if ($Receipt)
{
    if (Test-Path -LiteralPath $Receipt) { throw 'The requested receipt path already exists.' }
    $result | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $Receipt -Encoding utf8 -WhatIf:$false
}
$result | ConvertTo-Json -Depth 6
