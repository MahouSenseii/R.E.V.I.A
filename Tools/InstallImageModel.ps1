param(
    [string]$PythonVersion = '3.12',
    # sd-turbo is the default because it produces an image in 1-4 steps. On a machine
    # where the chat model already owns the GPU this worker lands on CPU, and a 30-step
    # model there takes minutes per picture rather than under one.
    [string]$Model = 'stabilityai/sd-turbo',
    [switch]$SkipModelDownload
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$runtimeRoot = Join-Path $repoRoot 'ThirdParty\ImageGen'
$venvRoot = Join-Path $runtimeRoot '.venv'
$pythonPath = Join-Path $venvRoot 'Scripts\python.exe'
$cacheRoot = Join-Path $runtimeRoot 'cache'
$launcher = Get-Command py.exe -ErrorAction SilentlyContinue

if ($null -eq $launcher) {
    throw 'Python Launcher was not found. Install 64-bit Python 3.12, then run this script again.'
}

New-Item -ItemType Directory -Force -Path $runtimeRoot | Out-Null
New-Item -ItemType Directory -Force -Path $cacheRoot | Out-Null

if (-not (Test-Path -LiteralPath $pythonPath -PathType Leaf)) {
    & $launcher.Source "-$PythonVersion" -m venv $venvRoot
    if ($LASTEXITCODE -ne 0) {
        throw "Could not create the image-generation Python $PythonVersion environment."
    }
}

& $pythonPath -m pip install --upgrade pip
if ($LASTEXITCODE -ne 0) {
    throw 'Could not update pip in the image-generation environment.'
}

# Same CUDA 12.8 wheels the voice runtime pins, for the same reason: they carry both
# Blackwell (RTX 50-series) and Turing (RTX 20-series) kernels, so one install works on
# the desktop's mixed pair and on the laptop.
& $pythonPath -m pip install torch==2.7.1 --index-url https://download.pytorch.org/whl/cu128
if ($LASTEXITCODE -ne 0) {
    throw 'Could not install the pinned PyTorch CUDA runtime.'
}

& $pythonPath -m pip install 'diffusers==0.31.0' 'transformers==4.46.3' 'accelerate==1.1.1' 'safetensors==0.4.5' 'pillow==11.0.0'
if ($LASTEXITCODE -ne 0) {
    throw 'Could not install the diffusers image stack.'
}

& $pythonPath -c "import torch, diffusers; print('image stack ready; torch:', torch.__version__, 'diffusers:', diffusers.__version__, 'CUDA:', torch.cuda.is_available())"
if ($LASTEXITCODE -ne 0) {
    throw 'The image runtime did not pass its import check.'
}

if (-not $SkipModelDownload) {
    Write-Host "Fetching $Model into $cacheRoot (this is the large step)..."
    $env:HF_HUB_DISABLE_TELEMETRY = '1'
    & $pythonPath -c "from diffusers import AutoPipelineForText2Image; AutoPipelineForText2Image.from_pretrained('$Model', cache_dir=r'$cacheRoot')"
    if ($LASTEXITCODE -ne 0) {
        throw "Could not download $Model. Re-run with -SkipModelDownload to install the runtime only."
    }
}

Write-Host ''
Write-Host "Image runtime is ready: $pythonPath"
Write-Host "Model: $Model"
Write-Host "Weights cached in: $cacheRoot"
Write-Host ''
Write-Host 'Check the model license before using output beyond personal work.'
Write-Host 'Set image.enabled true in Config/settings.json, then ask Revia to picture something.'
