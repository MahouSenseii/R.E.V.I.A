param(
    [switch]$Force,
    [switch]$SkipToolchain,
    [string]$Version = '6.8.3',
    [string]$InstallRoot = (Join-Path $env:USERPROFILE 'Qt')
)

# Qt is the only heavy dependency Revia could not install for itself, which made
# the desktop build silently unavailable on a clean machine. Qt's own online
# installer requires an account and a GUI, so it cannot be scripted. aqtinstall
# fetches the same official packages from Qt's mirrors without an account, which
# is how Qt-based CI systems provision their runners.
#
# The MinGW kit is mandatory. An MSVC kit is ABI-incompatible with the GCC
# toolchain this project builds with and will fail at link time.

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$kitPath = Join-Path $InstallRoot "$Version\mingw_64"
$venvRoot = Join-Path $repoRoot 'ThirdParty\QtInstaller'
$venvPython = Join-Path $venvRoot '.venv\Scripts\python.exe'
$aqtVersion = '3.1.*'

# Qt distributes its own build tools as separate "tool" packages. Installing
# them removes this project's dependency on CLion's bundled cmake/ninja/mingw.
$toolPackages = @(
    @{ Id = 'tools_mingw1310'; Purpose = 'MinGW 13.1.0 toolchain (ABI match for the Qt 6.8 mingw_64 kit)' },
    @{ Id = 'tools_ninja';     Purpose = 'Ninja generator' },
    @{ Id = 'tools_cmake';     Purpose = 'CMake' }
)

if ((Test-Path -LiteralPath (Join-Path $kitPath 'lib\cmake\Qt6\Qt6Config.cmake')) -and -not $Force) {
    Write-Host "Qt $Version is already installed: $kitPath"
    Write-Host 'Re-run with -Force to reinstall.'
    exit 0
}

function Resolve-Python {
    # Prefer the launcher because it resolves a real CPython even when the
    # Microsoft Store alias shadows python.exe with a stub that does nothing.
    $launcher = Get-Command -Name 'py.exe' -CommandType Application -ErrorAction SilentlyContinue
    if ($null -ne $launcher) {
        & $launcher.Source -3 -c 'import sys; sys.exit(0 if sys.version_info >= (3, 9) else 1)' 2>$null
        if ($LASTEXITCODE -eq 0) {
            return @($launcher.Source, '-3')
        }
    }

    $python = Get-Command -Name 'python.exe' -CommandType Application -ErrorAction SilentlyContinue
    if ($null -ne $python) {
        & $python.Source -c 'import sys; sys.exit(0 if sys.version_info >= (3, 9) else 1)' 2>$null
        if ($LASTEXITCODE -eq 0) {
            return @($python.Source)
        }
    }

    throw @'
Python 3.9 or newer is required to install Qt.
Install it from https://www.python.org/downloads/windows/ (tick "Add python.exe to PATH"),
then re-run this script. Alternatively install Qt manually and point CMake at it:
    cmake --preset debug -DREVIA_QT_ROOT=C:/Qt/6.8.3/mingw_64
'@
}

$pythonCommand = Resolve-Python
$pythonExe = $pythonCommand[0]
$pythonArgs = @($pythonCommand | Select-Object -Skip 1)

if ($Force -and (Test-Path -LiteralPath $venvRoot)) {
    Remove-Item -LiteralPath $venvRoot -Recurse -Force
}

if (-not (Test-Path -LiteralPath $venvPython)) {
    Write-Host 'Creating an isolated environment for the Qt installer...'
    New-Item -ItemType Directory -Path $venvRoot -Force | Out-Null
    & $pythonExe @pythonArgs -m venv (Join-Path $venvRoot '.venv')
    if ($LASTEXITCODE -ne 0) {
        throw "Could not create a virtual environment at $venvRoot\.venv (exit code $LASTEXITCODE)."
    }
}

Write-Host "Installing aqtinstall $aqtVersion..."
& $venvPython -m pip install --upgrade pip --quiet
if ($LASTEXITCODE -ne 0) {
    throw "pip could not be upgraded (exit code $LASTEXITCODE)."
}
& $venvPython -m pip install "aqtinstall==$aqtVersion" --quiet
if ($LASTEXITCODE -ne 0) {
    throw "aqtinstall could not be installed (exit code $LASTEXITCODE)."
}

New-Item -ItemType Directory -Path $InstallRoot -Force | Out-Null

Write-Host "Downloading Qt $Version (win64_mingw) into $InstallRoot..."
& $venvPython -m aqt install-qt windows desktop $Version win64_mingw --outputdir $InstallRoot
if ($LASTEXITCODE -ne 0) {
    throw @"
Qt $Version could not be installed (exit code $LASTEXITCODE).
List the versions Qt currently publishes with:
    $venvPython -m aqt list-qt windows desktop
"@
}

if (-not (Test-Path -LiteralPath (Join-Path $kitPath 'lib\cmake\Qt6\Qt6Config.cmake'))) {
    throw "The Qt installation completed but no usable kit is present at $kitPath."
}

if (-not $SkipToolchain) {
    foreach ($tool in $toolPackages) {
        Write-Host "Installing $($tool.Id) - $($tool.Purpose)..."
        & $venvPython -m aqt install-tool windows desktop $tool.Id --outputdir $InstallRoot
        if ($LASTEXITCODE -ne 0) {
            # Qt renames tool packages between releases, so a failure here is a
            # naming problem rather than a fatal one; the CLion toolchain still works.
            Write-Warning @"
Could not install $($tool.Id) (exit code $LASTEXITCODE).
Qt may have renamed this package. List the current tool IDs with:
    $venvPython -m aqt list-tool windows desktop
Revia will fall back to any toolchain already on PATH.
"@
        }
    }
}

Write-Host ''
Write-Host "Qt $Version is ready: $kitPath"
Write-Host 'CMake discovers this automatically. To override it explicitly, use:'
Write-Host "    cmake --preset debug -DREVIA_QT_ROOT=$($kitPath -replace '\\', '/')"
Write-Host ''
Write-Host 'Next:  .\Tools\Build.ps1'
