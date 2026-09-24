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

# CUDA 12.8 wheels include Blackwell/sm_120 kernels for RTX 50-series cards while
# retaining Turing support for cards such as the RTX 2070. The older cu121 wheel could
# import on a 5070 but had no compatible kernel image when synthesis actually started.
& $pythonPath -m pip install torch==2.7.1 torchaudio==2.7.1 --index-url https://download.pytorch.org/whl/cu128
if ($LASTEXITCODE -ne 0) {
    throw 'Could not install the pinned PyTorch CUDA runtime.'
}
& $pythonPath -m pip install qwen-tts==0.1.1
if ($LASTEXITCODE -ne 0) {
    throw 'Could not install Qwen3-TTS.'
}

& $pythonPath -c "import torch, qwen_tts; print('Qwen3-TTS ready; torch:', torch.__version__, 'CUDA:', torch.cuda.is_available(), 'architectures:', torch.cuda.get_arch_list() if torch.cuda.is_available() else []); [(print('CUDA kernel test:', i, torch.cuda.get_device_name(i), torch.cuda.get_device_capability(i), (torch.ones(1, device=f'cuda:{i}') + 1).item()), torch.cuda.synchronize(i)) for i in range(torch.cuda.device_count())]"
if ($LASTEXITCODE -ne 0) {
    throw 'The Qwen3-TTS runtime did not pass its import check.'
}

Write-Host "Qwen3-TTS runtime is ready: $pythonPath"
Write-Host 'Adaptive attention uses PyTorch SDPA on native-BF16 GPUs and the stable package default elsewhere.'
Write-Host 'flash-attn is intentionally optional and is not forced on Windows; benchmark it before selecting flash_attention_2.'
Write-Host 'Model weights download on the first Create voice and Generate preview operations.'
