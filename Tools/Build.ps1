param(
    [switch]$SkipTests
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot

function Resolve-ReviaTool {
    param(
        [Parameter(Mandatory = $true)]
        [string]$CommandName,

        [Parameter(Mandatory = $true)]
        [string[]]$Candidates
    )

    $command = Get-Command -Name $CommandName -CommandType Application -ErrorAction SilentlyContinue
    if ($null -ne $command) {
        return $command.Source
    }

    foreach ($candidate in $Candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return $candidate
        }
    }

    throw "Could not find $CommandName. Install CMake, Ninja, and a C++20 compiler or update Tools/Build.ps1."
}

$clionBin = Join-Path $env:LOCALAPPDATA 'Programs\CLion\bin'
$cmakePath = Resolve-ReviaTool -CommandName 'cmake.exe' -Candidates @(
    (Join-Path $clionBin 'cmake\win\x64\bin\cmake.exe')
)
$ctestPath = Join-Path (Split-Path -Parent $cmakePath) 'ctest.exe'
$ninjaPath = Resolve-ReviaTool -CommandName 'ninja.exe' -Candidates @(
    (Join-Path $clionBin 'ninja\win\x64\ninja.exe')
)
$compilerPath = Resolve-ReviaTool -CommandName 'g++.exe' -Candidates @(
    (Join-Path $clionBin 'mingw\bin\g++.exe')
)

$toolDirectories = @(
    (Split-Path -Parent $compilerPath),
    (Split-Path -Parent $ninjaPath),
    (Split-Path -Parent $cmakePath)
)
$env:Path = ($toolDirectories -join ';') + ';' + $env:Path

Push-Location $repoRoot
try {
    & $cmakePath --preset debug "-DCMAKE_CXX_COMPILER=$compilerPath" "-DCMAKE_MAKE_PROGRAM=$ninjaPath"
    if ($LASTEXITCODE -ne 0) {
        throw "CMake configuration failed with exit code $LASTEXITCODE."
    }

    & $cmakePath --build --preset debug --parallel
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed with exit code $LASTEXITCODE."
    }

    if (-not $SkipTests) {
        & $ctestPath --preset debug
        if ($LASTEXITCODE -ne 0) {
            throw "Tests failed with exit code $LASTEXITCODE."
        }
    }

    Write-Host "REVIA CLI is ready: $repoRoot\build\debug\R_E_V_I_A.exe"
    $desktopPath = Join-Path $repoRoot 'build\debug\ReviaDesktop.exe'
    if (Test-Path -LiteralPath $desktopPath -PathType Leaf) {
        Write-Host "REVIA Desktop is ready: $desktopPath"
    }
}
finally {
    Pop-Location
}
