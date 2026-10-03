param(
    [Parameter(Mandatory=$true)][string]$SourceRoot,
    [Parameter(Mandatory=$true)][string]$BuildRoot,
    [Parameter(Mandatory=$true)][string]$TestOutput,
    [Parameter(Mandatory=$true)][string]$ResultPath,
    [Parameter(Mandatory=$true)][string]$DiscoveryOutput,
    [string]$DepsRoot='',
    [int]$Jobs=2
)

$ErrorActionPreference='Stop'
function Get-ArtifactDigest([string]$Path)
{
    $stream=[IO.File]::Open($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
    try
    {
        $sha=[Security.Cryptography.SHA256]::Create()
        try {return [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-','')}
        finally {$sha.Dispose()}
    }
    finally {$stream.Dispose()}
}

$started=[DateTime]::UtcNow
$scratchReceipt=''
$result=[ordered]@{
    built=$false;testsRan=$false;configureExitCode=-1;buildExitCode=-1;discoveryExitCode=-1;testExitCode=-1;seconds=0.0
    requestedBuildRoot='';nativeBuildRoot='';usedShortScratch=$false;scratchOwned=$false
    artifactPath='';artifactSha256='';nativeArtifactSha256='';ctestFileSha256='';nativeCtestFileSha256=''
}
$exitCode=1
try
{
    $candidateHeader=Join-Path $SourceRoot 'Desktop/studioDuration.h'
    $bytes=[IO.File]::ReadAllText($candidateHeader)
    $precisionMatch=[regex]::Match($bytes, "QString::number\(static_cast<double>\(milliseconds\) / 1000\.0, 'f', ([123])\)")
    if(-not $precisionMatch.Success){throw 'Candidate has no admitted precision parameter.'}
    $qtKit=$env:REVIA_QT_ROOT
    if(-not $qtKit)
    {
        $kitRoots=@('C:/Qt',(Join-Path $env:USERPROFILE 'Qt'))
        $kits=@(foreach($kitRoot in $kitRoots)
        {
            if(Test-Path -LiteralPath $kitRoot)
            {
                Get-ChildItem -LiteralPath $kitRoot -Directory | Where-Object {$_.Name -match '^6\.\d+\.\d+$'} |
                    ForEach-Object {Join-Path $_.FullName 'mingw_64'} | Where-Object {Test-Path -LiteralPath (Join-Path $_ 'bin/qmake.exe')}
            }
        })
        if($kits.Count -eq 0){throw 'No Qt MinGW kit is available for the closed presentation check.'}
        $qtKit=$kits | Sort-Object -Descending | Select-Object -First 1
    }
    $toolRoot=Split-Path -Parent $qtKit
    $mingwRoot=Join-Path (Split-Path -Parent $toolRoot) 'Tools/mingw1310_64/bin'
    if(-not (Test-Path -LiteralPath (Join-Path $mingwRoot 'g++.exe'))){throw 'The Qt MinGW compiler is unavailable.'}
    $env:PATH=(Join-Path $qtKit 'bin')+';'+$mingwRoot+';'+$env:PATH
    $trustedProject=Join-Path $PSScriptRoot 'Studio'
    $BuildRoot=[IO.Path]::GetFullPath($BuildRoot)
    $nativeBuildRoot=$BuildRoot
    $result.requestedBuildRoot=$BuildRoot
    New-Item -ItemType Directory -Force -Path $BuildRoot | Out-Null
    # CMake's compiler probes append about 100 characters; keep their native paths below Windows tool limits.
    if($BuildRoot.Length -gt 140)
    {
        $scratchDrive=[IO.Path]::GetPathRoot([IO.Path]::GetTempPath())
        $nativeBuildRoot=Join-Path $scratchDrive ('rv6-v-'+[Guid]::NewGuid().ToString('N').Substring(0,12))
        New-Item -ItemType Directory -Path $nativeBuildRoot | Out-Null
        $result.usedShortScratch=$true
        $result.scratchOwned=$true
    }
    $result.nativeBuildRoot=$nativeBuildRoot
    $scratchReceipt=Join-Path $BuildRoot 'native-scratch.json'
    $result | ConvertTo-Json | Set-Content -Encoding utf8 -LiteralPath $scratchReceipt
    & cmake -S $trustedProject -B $nativeBuildRoot -G Ninja "-DCMAKE_CXX_COMPILER=$mingwRoot/g++.exe" "-DCMAKE_PREFIX_PATH=$qtKit" "-DREVIA_CANDIDATE_ROOT=$SourceRoot" "-DREVIA_EXPECTED_DIGITS=$($precisionMatch.Groups[1].Value)"
    $result.configureExitCode=$LASTEXITCODE
    if($result.configureExitCode -ne 0){throw 'Closed presentation configuration failed.'}
    & cmake --build $nativeBuildRoot --parallel ([Math]::Max(1,[Math]::Min(4,$Jobs)))
    $result.buildExitCode=$LASTEXITCODE
    if($result.buildExitCode -ne 0){throw 'Closed presentation build failed.'}
    $nativeArtifact=Join-Path $nativeBuildRoot 'ReviaStudioDuration.exe'
    $artifact=Join-Path $BuildRoot 'ReviaStudioDuration.exe'
    $nativeCtestFile=Join-Path $nativeBuildRoot 'CTestTestfile.cmake'
    $ctestFile=Join-Path $BuildRoot 'CTestTestfile.cmake'
    $result.nativeArtifactSha256=Get-ArtifactDigest $nativeArtifact
    $result.nativeCtestFileSha256=Get-ArtifactDigest $nativeCtestFile
    if($result.usedShortScratch)
    {
        Copy-Item -LiteralPath $nativeArtifact -Destination $artifact
        # Relocate only host-generated CTest metadata so discovery, execution and later probes use the retained exact artifact.
        $metadata=[IO.File]::ReadAllText($nativeCtestFile).Replace($nativeBuildRoot.Replace('\','/'),$BuildRoot.Replace('\','/'))
        [IO.File]::WriteAllText($ctestFile,$metadata,(New-Object Text.UTF8Encoding($false)))
    }
    $result.artifactPath=$artifact
    $result.artifactSha256=Get-ArtifactDigest $artifact
    $result.ctestFileSha256=Get-ArtifactDigest $ctestFile
    if($result.artifactSha256 -ne $result.nativeArtifactSha256){throw 'Closed presentation artifact copy did not match its native build.'}
    $result.built=$true
    # Registry receipts bind native bytes; PowerShell's text pipeline changes their line endings.
    $discoveryProcess=New-Object Diagnostics.Process
    $discoveryProcess.StartInfo.FileName=(Get-Command ctest -CommandType Application | Select-Object -First 1).Source
    $discoveryProcess.StartInfo.Arguments='--test-dir "'+$BuildRoot.Replace('\','/')+'" --show-only=json-v1'
    $discoveryProcess.StartInfo.UseShellExecute=$false
    $discoveryProcess.StartInfo.CreateNoWindow=$true
    $discoveryProcess.StartInfo.RedirectStandardOutput=$true
    $discoveryProcess.StartInfo.RedirectStandardError=$true
    $discoveryStream=[IO.File]::Open($DiscoveryOutput,[IO.FileMode]::Create,[IO.FileAccess]::Write,[IO.FileShare]::Read)
    try
    {
        if(-not $discoveryProcess.Start()){throw 'Closed presentation test discovery could not start.'}
        $discoveryErrors=$discoveryProcess.StandardError.ReadToEndAsync()
        $discoveryProcess.StandardOutput.BaseStream.CopyTo($discoveryStream)
        $discoveryProcess.WaitForExit()
        $result.discoveryExitCode=$discoveryProcess.ExitCode
        $diagnostics=$discoveryErrors.GetAwaiter().GetResult()
        if($diagnostics){Write-Output $diagnostics}
    }
    finally
    {
        $discoveryStream.Dispose()
        $discoveryProcess.Dispose()
    }
    if($result.discoveryExitCode -ne 0){throw 'Closed presentation test discovery failed.'}
    & ctest --test-dir $BuildRoot --output-on-failure 2>&1 | Tee-Object -FilePath $TestOutput
    $result.testExitCode=$LASTEXITCODE
    $result.testsRan=$true
    if($result.testExitCode -eq 0){$exitCode=0}
}
catch {Write-Output $_.Exception.Message}
finally
{
    $result.seconds=([DateTime]::UtcNow-$started).TotalSeconds
    if($scratchReceipt){$result | ConvertTo-Json | Set-Content -Encoding utf8 -LiteralPath $scratchReceipt}
    $result | ConvertTo-Json | Set-Content -Encoding utf8 -LiteralPath $ResultPath
}
exit $exitCode
