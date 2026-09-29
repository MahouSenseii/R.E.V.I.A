param(
    [string]$PythonVersion = '3.12'
)

# The singing toolchain: vocal separation and voice conversion for songs made with Suno.
#
# Its own environment, like the image and voice runtimes, because the separator and RVC
# pin their own PyTorch and neither should be able to break the voice that speaks. The
# models themselves are fetched on first use by the tools (the RoFormer separator
# weights) or trained by you (the RVC model of Revia's voice; see Tools/Singing/README.md).
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$runtimeRoot = Join-Path $repoRoot 'ThirdParty\Singing'
$venvRoot = Join-Path $runtimeRoot '.venv'
$pythonPath = Join-Path $venvRoot 'Scripts\python.exe'
$launcher = Get-Command py.exe -ErrorAction SilentlyContinue

if ($null -eq $launcher) {
    throw 'Python Launcher was not found. Install 64-bit Python 3.12, then run this script again.'
}

New-Item -ItemType Directory -Force -Path $runtimeRoot | Out-Null

if (-not (Test-Path -LiteralPath $pythonPath -PathType Leaf)) {
    & $launcher.Source "-$PythonVersion" -m venv $venvRoot
    if ($LASTEXITCODE -ne 0) {
        throw "Could not create the singing Python $PythonVersion environment."
    }
}

& $pythonPath -m pip install --upgrade pip
if ($LASTEXITCODE -ne 0) {
    throw 'Could not update pip in the singing environment.'
}

# The same CUDA 12.8 wheels the voice runtime pins, for the same reason: one install
# works on the desktop's mixed pair of cards and on a laptop.
& $pythonPath -m pip install torch==2.7.1 torchaudio==2.7.1 --index-url https://download.pytorch.org/whl/cu128
if ($LASTEXITCODE -ne 0) {
    throw 'Could not install the pinned PyTorch CUDA runtime.'
}

& $pythonPath -m pip install 'audio-separator[gpu]' 'rvc-python' 'soundfile' 'numpy'
if ($LASTEXITCODE -ne 0) {
    throw 'Could not install the separation and voice-conversion stack.'
}

& $pythonPath (Join-Path $repoRoot 'Tools\Singing\prepare_song.py') --check
if ($LASTEXITCODE -ne 0) {
    throw 'The singing tools did not pass their check.'
}

Write-Host ''
Write-Host "Singing tools are ready: $pythonPath"
Write-Host 'Next: train an RVC model of her voice (Tools/Singing/README.md), then'
Write-Host '  prepare_song.py --input song.mp3 --title "..." --songs-root <RuntimeData/Songs> --voice-model revia.pth'
