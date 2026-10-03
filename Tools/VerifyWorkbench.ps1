# Builds and tests Revia's improvement workbench: a private copy of her source with one
# proposed change applied. Started by the running app, never by hand; it writes only
# under -BuildRoot, -TestOutput and -ResultPath.
#
# The result file is the contract with the app:
#   built/testsRan, discoveryExitCode/testExitCode, seconds; discovery is separate JSON.
# Everything else goes to standard output, which the app keeps as the build log.
param(
    [Parameter(Mandatory = $true)][string]$SourceRoot,
    [Parameter(Mandatory = $true)][string]$BuildRoot,
    [Parameter(Mandatory = $true)][string]$TestOutput,
    [Parameter(Mandatory = $true)][string]$ResultPath,
    [Parameter(Mandatory = $true)][string]$DiscoveryOutput,
    [string]$DepsRoot = '',
    [int]$Jobs = 4
)

$ErrorActionPreference = 'Stop'
$started = Get-Date
$discoveryExitCode = -1
$testExitCode = -1

function Write-Result([bool]$built, [bool]$testsRan) {
    $seconds = [math]::Round(((Get-Date) - $started).TotalSeconds, 1)
    $json = @{ built = $built; testsRan = $testsRan; seconds = $seconds;
        discoveryExitCode = $discoveryExitCode; testExitCode = $testExitCode } | ConvertTo-Json -Compress
    Set-Content -LiteralPath $ResultPath -Value $json -Encoding UTF8
}

# The same toolchain Tools/Build.ps1 finds, in the same order: PATH, then Qt's own tools,
# then CLion's bundled copies. Kept in step with Build.ps1 by hand; it is short.
function Find-Tool([string]$name, [string[]]$candidates) {
    $command = Get-Command -Name $name -CommandType Application -ErrorAction SilentlyContinue
    if ($null -ne $command) { return @($command)[0].Source }
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
    }
    return $null
}

$clionBin = Join-Path $env:LOCALAPPDATA 'Programs\CLion\bin'
$qtTools = Join-Path $env:USERPROFILE 'Qt\Tools'
$cmake = Find-Tool 'cmake.exe' @((Join-Path $qtTools 'CMake_64\bin\cmake.exe'), (Join-Path $clionBin 'cmake\win\x64\bin\cmake.exe'))
$ninja = Find-Tool 'ninja.exe' @((Join-Path $qtTools 'Ninja\ninja.exe'), (Join-Path $clionBin 'ninja\win\x64\ninja.exe'))
$gxx = Find-Tool 'g++.exe' @((Join-Path $qtTools 'mingw1310_64\bin\g++.exe'), (Join-Path $clionBin 'mingw\bin\g++.exe'))
if (-not $cmake -or -not $ninja -or -not $gxx) {
    Write-Output "The toolchain was not found (cmake: $cmake, ninja: $ninja, g++: $gxx)."
    Write-Result $false $false
    exit 1
}
$gcc = Join-Path (Split-Path -Parent $gxx) 'gcc.exe'
$ctest = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
$env:Path = ((Split-Path -Parent $gxx), (Split-Path -Parent $ninja), (Split-Path -Parent $cmake) -join ';') + ';' + $env:Path
Write-Output "CMake: $cmake"
Write-Output "C++:   $gxx"

# An interrupted archive step leaves an 8-byte library newer than its objects, which
# ninja then never rebuilds. A cancelled proof is exactly such an interruption.
foreach ($name in @('libReviaFoundation.a', 'libsqlite3.a')) {
    $archive = Join-Path $BuildRoot $name
    if ((Test-Path -LiteralPath $archive -PathType Leaf) -and ((Get-Item -LiteralPath $archive).Length -lt 1MB)) {
        Write-Output "Removing a truncated $name so it is rebuilt."
        Remove-Item -LiteralPath $archive -Force
    }
}

# Debug semantics (no optimisation, assertions on) without debug information. Nobody
# attaches a debugger to a proof, and the debug information is what made the library a
# 565 MB archive that every test executable then spent minutes linking against.
$configure = @(
    '-S', $SourceRoot, '-B', $BuildRoot, '-G', 'Ninja',
    '-DCMAKE_BUILD_TYPE=Debug', '-DBUILD_TESTING=ON',
    '-DCMAKE_CXX_FLAGS_DEBUG=-O0', '-DCMAKE_C_FLAGS_DEBUG=-O0',
    "-DCMAKE_C_COMPILER=$gcc", "-DCMAKE_CXX_COMPILER=$gxx", "-DCMAKE_MAKE_PROGRAM=$ninja")
# The app's own build already downloaded every dependency; reuse those sources so a proof
# never needs the network.
if ($DepsRoot -and (Test-Path -LiteralPath $DepsRoot -PathType Container)) {
    $allPresent = $true
    foreach ($dependency in @('httplib', 'json', 'sqlite', 'expat')) {
        $sourceDir = Join-Path $DepsRoot "$dependency-src"
        if (Test-Path -LiteralPath $sourceDir -PathType Container) {
            $configure += "-DFETCHCONTENT_SOURCE_DIR_$($dependency.ToUpperInvariant())=$sourceDir"
        } else {
            $allPresent = $false
        }
    }
    if ($allPresent) { $configure += '-DFETCHCONTENT_FULLY_DISCONNECTED=ON' }
}

$ErrorActionPreference = 'Continue'
& $cmake @configure
$configureExitCode = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
if ($configureExitCode -ne 0) {
    Write-Output "Configuration failed with exit code $configureExitCode."
    Write-Result $false $false
    exit 1
}

$ErrorActionPreference = 'Continue'
& $cmake --build $BuildRoot --parallel $Jobs
$buildExitCode = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
if ($buildExitCode -ne 0) {
    Write-Output "The build failed with exit code $buildExitCode."
    Write-Result $false $false
    exit 1
}

$ErrorActionPreference = 'Continue'
$discoveryLines = @(& $ctest --test-dir $BuildRoot --show-only=json-v1)
$discoveryExitCode = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
$discoveryText = $discoveryLines -join "`n"
[System.IO.File]::WriteAllText($DiscoveryOutput, $discoveryText)
if ($discoveryExitCode -ne 0) {
    Write-Output "CTest discovery failed with exit code $discoveryExitCode."
    Write-Result $true $false
    exit 1
}
try {
    $discovered = ConvertFrom-Json -InputObject $discoveryText -ErrorAction Stop
    if ($discovered.kind -ne 'ctestInfo' -or $discovered.version.major -ne 1 -or
        $discovered.version.minor -ne 0 -or $discovered.tests -isnot [array] -or
        $discovered.tests.Count -eq 0) {
        throw 'CTest discovery did not report a nonempty registry.'
    }
} catch {
    Write-Output "CTest discovery was invalid: $_"
    Write-Result $true $false
    exit 1
}

# CTest's summary lines are what the app reads, so they go to their own file as well as
# the log. Written as UTF-8 by .NET directly: Windows PowerShell's Tee-Object and
# Out-File write UTF-16, which the app would read as noise.
# Windows PowerShell treats native stderr as error records. It must not interrupt a
# complete failing test run before its native exit code and terminal rows are saved.
$ErrorActionPreference = 'Continue'
$testLines = @(& $ctest --test-dir $BuildRoot --output-on-failure --timeout 900 2>&1)
$testExitCode = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
$testLines = @($testLines | ForEach-Object { "$_" })
$testLines | ForEach-Object { Write-Output $_ }
[System.IO.File]::WriteAllLines($TestOutput, [string[]]$testLines)
Write-Result $true $true
exit 0
