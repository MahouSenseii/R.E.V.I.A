param(
    [string]$PythonVersion = '3.12'
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$runtimeRoot = Join-Path $repoRoot 'ThirdParty\QwenTTS'
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
        throw "Could not create the Qwen3-TTS Python $PythonVersion environment."
    }
}

& $pythonPath -m pip install --upgrade pip
if ($LASTEXITCODE -ne 0) {
    throw 'Could not update pip in the Qwen3-TTS environment.'
}

# This is the CUDA/Python combination validated with Revia's local worker.
& $pythonPath -m pip install torch==2.5.1 torchaudio==2.5.1 --index-url https://download.pytorch.org/whl/cu121
if ($LASTEXITCODE -ne 0) {
    throw 'Could not install the pinned PyTorch CUDA runtime.'
}
& $pythonPath -m pip install qwen-tts==0.1.1
if ($LASTEXITCODE -ne 0) {
    throw 'Could not install Qwen3-TTS.'
}

& $pythonPath -c "import torch, qwen_tts; print('Qwen3-TTS ready; CUDA:', torch.cuda.is_available())"
if ($LASTEXITCODE -ne 0) {
    throw 'The Qwen3-TTS runtime did not pass its import check.'
}

Write-Host "Qwen3-TTS runtime is ready: $pythonPath"
Write-Host 'Model weights download on the first Create voice and Generate preview operations.'
