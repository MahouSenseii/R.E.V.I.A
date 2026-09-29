param(
    [string]$PythonVersion = '3.12',
    # A Kokoro voice id. The first letter is the language: a = American English,
    # b = British English, j = Japanese, z = Mandarin, e = Spanish, f = French,
    # h = Hindi, i = Italian, p = Brazilian Portuguese.
    [string]$Voice = 'af_heart',
    [switch]$SkipEspeak
)

# Installs Kokoro-82M (Apache-2.0) as Revia's fallback voice: the one that speaks a
# phrase when every Qwen3-TTS worker is down or its card is full, before Windows SAPI.
# Its own environment, CPU-only on purpose -- the fallback exists for the moment the
# GPU cannot be relied on, so it must not need it. Kokoro is faster than real time on
# a CPU. Nothing here touches the Qwen environment.

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'ReviaAcceleration.ps1') # Save-ReviaJsonFile
$runtimeRoot = Join-Path $repoRoot 'ThirdParty\Kokoro'
$venvRoot = Join-Path $runtimeRoot '.venv'
$pythonPath = Join-Path $venvRoot 'Scripts\python.exe'
$launcher = Get-Command py.exe -ErrorAction SilentlyContinue

if ($null -eq $launcher) {
    throw 'Python Launcher was not found. Install 64-bit Python 3.12, then run this script again.'
}
if ($Voice -notmatch '^[A-Za-z0-9_-]+$') {
    throw "'$Voice' is not a Kokoro voice id (letters, digits, '_' and '-' only)."
}

New-Item -ItemType Directory -Force -Path $runtimeRoot | Out-Null
if (-not (Test-Path -LiteralPath $pythonPath -PathType Leaf)) {
    & $launcher.Source "-$PythonVersion" -m venv $venvRoot
    if ($LASTEXITCODE -ne 0) {
        throw "Could not create the Kokoro Python $PythonVersion environment."
    }
}

& $pythonPath -m pip install --upgrade pip
if ($LASTEXITCODE -ne 0) {
    throw 'Could not update pip in the Kokoro environment.'
}
# CPU wheels, about 200 MB, instead of the 3 GB CUDA build the Qwen environment
# carries. See the note at the top: this voice must work with the GPU gone.
& $pythonPath -m pip install torch==2.7.1 --index-url https://download.pytorch.org/whl/cpu
if ($LASTEXITCODE -ne 0) {
    throw 'Could not install the CPU PyTorch runtime for Kokoro.'
}
& $pythonPath -m pip install 'kokoro>=0.9.4' soundfile
if ($LASTEXITCODE -ne 0) {
    throw 'Could not install Kokoro.'
}
if ($Voice -match '^[jz]') {
    # Japanese and Mandarin need their own grapheme-to-phoneme packages.
    $extra = if ($Voice -match '^j') { 'misaki[ja]' } else { 'misaki[zh]' }
    & $pythonPath -m pip install $extra
    if ($LASTEXITCODE -ne 0) {
        throw "Could not install $extra for the $Voice voice."
    }
}

# espeak-ng: Kokoro's English G2P falls back to it for words outside its dictionary,
# and every language other than English, Japanese and Mandarin depends on it. Without
# it English still works and an unknown word is skipped with a warning in
# Logs\qwen-tts-<port>.stderr.log, so this is best effort rather than a failure.
if (-not $SkipEspeak) {
    $winget = Get-Command winget.exe -ErrorAction SilentlyContinue
    if ($null -ne $winget) {
        & $winget.Source install --id eSpeak-NG.eSpeak-NG -e --silent --accept-package-agreements --accept-source-agreements
        if ($LASTEXITCODE -ne 0) {
            Write-Warning 'espeak-ng did not install (winget returned an error). English keeps working; rerun with winget later or -SkipEspeak to silence this.'
        }
    } else {
        Write-Warning 'winget was not found, so espeak-ng was not installed. English keeps working.'
    }
}

& $pythonPath -c "from kokoro import KPipeline; import torch; print('Kokoro ready; torch:', torch.__version__)"
if ($LASTEXITCODE -ne 0) {
    throw 'The Kokoro runtime did not pass its import check.'
}

# Turn it on. The service reads these at the next start.
$settingsPath = Join-Path $repoRoot 'Config\settings.json'
if (Test-Path -LiteralPath $settingsPath -PathType Leaf) {
    $settings = Get-Content -LiteralPath $settingsPath -Raw | ConvertFrom-Json
    if ($null -eq $settings.speech) {
        $settings | Add-Member -NotePropertyName 'speech' -NotePropertyValue ([pscustomobject]@{}) -Force
    }
    foreach ($pair in @(
        @{ Name = 'fallbackVoiceEnabled'; Value = $true },
        @{ Name = 'fallbackVoiceScript'; Value = 'Tools/kokoro_tts_service.py' },
        @{ Name = 'fallbackVoicePythonExecutable'; Value = 'ThirdParty/Kokoro/.venv/Scripts/python.exe' },
        @{ Name = 'fallbackVoicePort'; Value = 8097 },
        @{ Name = 'fallbackVoice'; Value = $Voice })) {
        $settings.speech | Add-Member -NotePropertyName $pair.Name -NotePropertyValue $pair.Value -Force
    }
    Save-ReviaJsonFile -Path $settingsPath -Value $settings
    Write-Host "Config/settings.json: speech.fallbackVoiceEnabled = true, voice $Voice on port 8097."
} else {
    Write-Warning "Config/settings.json was not found; set speech.fallbackVoiceEnabled to true yourself once it exists."
}

Write-Host "Kokoro fallback voice is ready: $pythonPath"
Write-Host 'The voice model (hexgrad/Kokoro-82M, about 330 MB) downloads from Hugging Face the first time she needs it.'
Write-Host 'Try it without breaking anything: stop the Qwen worker while she is talking, or set speech.qwenPort to a closed port for one session.'
