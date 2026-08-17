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

    throw @"
Could not find $CommandName.
Revia looks on PATH first, then in a Qt Tools installation, then in CLion's bundled tools.
Install a toolchain with:
    .\Tools\InstallQt.ps1
That provides CMake, Ninja, and an ABI-matched MinGW 13.1.0 compiler alongside Qt.
"@
}

# Toolchain discovery must not assume CLion is installed. Tools/InstallQt.ps1
# provisions Qt's own CMake, Ninja, and MinGW packages, so those are checked
# before falling back to CLion's bundled copies.
$clionBin = Join-Path $env:LOCALAPPDATA 'Programs\CLion\bin'
$qtTools = Join-Path $env:USERPROFILE 'Qt\Tools'

$cmakePath = Resolve-ReviaTool -CommandName 'cmake.exe' -Candidates @(
    (Join-Path $qtTools 'CMake_64\bin\cmake.exe'),
    (Join-Path $clionBin 'cmake\win\x64\bin\cmake.exe')
)
$ctestPath = Join-Path (Split-Path -Parent $cmakePath) 'ctest.exe'
$ninjaPath = Resolve-ReviaTool -CommandName 'ninja.exe' -Candidates @(
    (Join-Path $qtTools 'Ninja\ninja.exe'),
    (Join-Path $clionBin 'ninja\win\x64\ninja.exe')
)
$compilerPath = Resolve-ReviaTool -CommandName 'g++.exe' -Candidates @(
    (Join-Path $qtTools 'mingw1310_64\bin\g++.exe'),
    (Join-Path $clionBin 'mingw\bin\g++.exe')
)

# Printed because an ABI mismatch between the compiler and the Qt kit produces
# link errors that are otherwise very hard to attribute.
Write-Host "CMake:    $cmakePath"
Write-Host "Ninja:    $ninjaPath"
Write-Host "Compiler: $compilerPath"

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
    else {
        # Previously this branch printed nothing, so a CLI-only build looked
        # identical to a successful one.
        Write-Warning @"
ReviaDesktop.exe was NOT built - Qt was not found during configuration.
Install Qt and reconfigure:
    .\Tools\InstallQt.ps1
    Remove-Item -Recurse -Force .\build\debug
    .\Tools\Build.ps1
"@
    }
}
finally {
    Pop-Location
}
