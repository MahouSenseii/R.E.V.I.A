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

# The project declares LANGUAGES C CXX because sqlite3.c is compiled as C, so
# CMake needs both compilers. Passing only CMAKE_CXX_COMPILER lets CMake choose
# a C compiler by its own search, which can land in a different MinGW
# installation than the C++ compiler came from. Two MinGW distributions
# reachable at once break the compiler driver with no diagnostic at all: the
# try-compile fails with an empty error block. Always take gcc.exe from beside
# the g++.exe that was selected.
$compilerDirectory = Split-Path -Parent $compilerPath
$cCompilerPath = Join-Path $compilerDirectory 'gcc.exe'
if (-not (Test-Path -LiteralPath $cCompilerPath -PathType Leaf)) {
    throw @"
Found g++.exe at $compilerPath but no gcc.exe beside it.
Revia needs a C and a C++ compiler from the same toolchain.
Install a complete toolchain with:
    .\Tools\InstallQt.ps1
"@
}

# Printed because an ABI mismatch between the compiler and the Qt kit produces
# link errors that are otherwise very hard to attribute.
Write-Host "CMake:        $cmakePath"
Write-Host "Ninja:        $ninjaPath"
Write-Host "C compiler:   $cCompilerPath"
Write-Host "C++ compiler: $compilerPath"

$toolDirectories = @(
    (Split-Path -Parent $compilerPath),
    (Split-Path -Parent $ninjaPath),
    (Split-Path -Parent $cmakePath)
)
$env:Path = ($toolDirectories -join ';') + ';' + $env:Path

# ---------------------------------------------------------------------------
# Static-archive sanity.
#
# An interrupted or blocked 'ar' leaves libReviaFoundation.a as an 8-byte header
# with zero members, and - this is the part that bites - with an mtime NEWER than
# every object that should be in it. Make and Ninja both then treat the archive
# as up to date and skip the archive step on every later build, so the tree stays
# broken no matter how many times you rebuild. The symptom is ~90 lines of
# undefined references to symbols whose .cpp files compiled perfectly.
#
# CMake's generated makefiles do not set .DELETE_ON_ERROR, so the partial file
# survives. Removing it before configuring is what actually breaks the loop;
# checking it afterwards is what stops the next occurrence from costing an hour.
#
# 1 MiB floor: a real Debug build of this library is several hundred MB, and the
# broken case is 8 bytes. Nothing legitimate lands between.
$reviaMinimumArchiveBytes = 1MB
$reviaStaticLibraries = @('libReviaFoundation.a', 'libsqlite3.a')

function Test-ReviaPoisonedArchive {
    param([Parameter(Mandatory = $true)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
    return ((Get-Item -LiteralPath $Path).Length -lt $reviaMinimumArchiveBytes)
}

function Repair-ReviaBuildTree {
    # Removes poisoned archives so the next build recreates them. Objects are
    # untouched, so the repair costs one archive step and one link, not a rebuild.
    param([Parameter(Mandatory = $true)][string]$BuildDirectory)
    if (-not (Test-Path -LiteralPath $BuildDirectory -PathType Container)) { return }
    foreach ($name in $reviaStaticLibraries) {
        $archive = Join-Path $BuildDirectory $name
        if (Test-ReviaPoisonedArchive -Path $archive) {
            $size = (Get-Item -LiteralPath $archive).Length
            Write-Warning "Removing a truncated static library so it can be rebuilt: $name ($size bytes)."
            Remove-Item -LiteralPath $archive -Force
        }
    }
}

function Assert-ReviaBuildTree {
    # Runs after the build. A truncated archive here means the build reported
    # success while producing something that cannot link.
    param([Parameter(Mandatory = $true)][string]$BuildDirectory)
    foreach ($name in $reviaStaticLibraries) {
        $archive = Join-Path $BuildDirectory $name
        if (Test-ReviaPoisonedArchive -Path $archive) {
            $size = (Get-Item -LiteralPath $archive).Length
            throw @"
$name is $size bytes - it has no members, and anything linking against it will
fail with undefined references to code that compiled fine.

The archiver was interrupted or the file was locked while being written. The
archive now has a newer timestamp than its objects, so the build system will
skip rebuilding it forever.

Fix:
    Remove-Item -LiteralPath '$archive' -Force
    .\Tools\Build.ps1

If this keeps happening, the build tree is being locked by something else while
the compiler writes to it - OneDrive syncing the build directory is the usual
cause on this machine.
"@
        }
    }
}


# A poisoned archive from an earlier interrupted build survives with a fresh
# timestamp, so clear it before configuring rather than after failing to link.
Repair-ReviaBuildTree -BuildDirectory (Join-Path $repoRoot 'build\debug')

# CLion keeps its own tree with its own generator and toolchain. This script does
# not build it, but a poisoned archive there produces the identical link failure
# and is far harder to attribute, so say so plainly.
$clionTree = Join-Path $repoRoot 'cmake-build-debug'
foreach ($name in $reviaStaticLibraries) {
    if (Test-ReviaPoisonedArchive -Path (Join-Path $clionTree $name)) {
        Write-Warning "$name is also truncated in cmake-build-debug (CLion's tree). Delete it there, or build from CLion once after this script."
    }
}

Push-Location $repoRoot
try {
    & $cmakePath --preset debug `
        "-DCMAKE_C_COMPILER=$cCompilerPath" `
        "-DCMAKE_CXX_COMPILER=$compilerPath" `
        "-DCMAKE_MAKE_PROGRAM=$ninjaPath"
    if ($LASTEXITCODE -ne 0) {
        throw "CMake configuration failed with exit code $LASTEXITCODE."
    }

    & $cmakePath --build --preset debug --parallel
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed with exit code $LASTEXITCODE."
    }

    # A zero exit code is not proof the tree is linkable.
    Assert-ReviaBuildTree -BuildDirectory (Join-Path $repoRoot 'build\debug')

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
