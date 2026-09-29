param(
    [string]$PythonVersion = '3.12'
)

# Installs the speaker-embedding worker: sherpa-onnx (Apache-2.0) in its own CPU-only
# environment and the WeSpeaker CAM++ model (VoxCeleb, English, 16 kHz) it runs, then
# turns speechRecognition.speakerIdentificationEnabled on. What this buys: each thing
# she hears also becomes a voice embedding, and the people who asked her to remember
# their voice ("Revia, remember my voice, I'm Sam" or /voice enroll Sam) are recognised
# at the microphone. The embeddings never leave the machine; the voiceprints live in
# the DPAPI secret store; a match moves conversation attribution and nothing else.

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'ReviaAcceleration.ps1') # Save-ReviaJsonFile
$runtimeRoot = Join-Path $repoRoot 'ThirdParty\SpeakerId'
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
        throw "Could not create the speaker-id Python $PythonVersion environment."
    }
}

& $pythonPath -m pip install --upgrade pip
if ($LASTEXITCODE -ne 0) {
    throw 'Could not update pip in the speaker-id environment.'
}
& $pythonPath -m pip install sherpa-onnx==1.13.8
if ($LASTEXITCODE -ne 0) {
    throw 'Could not install sherpa-onnx.'
}

# The model, pinned by size and SHA-256 like every model file she runs.
$models = Join-Path $repoRoot 'Models'
New-Item -ItemType Directory -Force -Path $models | Out-Null
$modelPath = Join-Path $models 'wespeaker_en_voxceleb_CAM++.onnx'
$expectedSha256 = 'c46fad10b5f81e1aa4a60c162714208577093655076c5450f8c469e522ec54ef'
$expectedBytes = 29292684
$url = 'https://github.com/k2-fsa/sherpa-onnx/releases/download/speaker-recongition-models/wespeaker_en_voxceleb_CAM%2B%2B.onnx'
function Test-ReviaSpeakerModel {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
    if ((Get-Item -LiteralPath $Path).Length -ne $expectedBytes) { return $false }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() -eq $expectedSha256
}
if (Test-ReviaSpeakerModel -Path $modelPath) {
    Write-Host 'Verified: wespeaker_en_voxceleb_CAM++.onnx'
} else {
    $partial = "$modelPath.partial"
    Write-Host 'Downloading wespeaker_en_voxceleb_CAM++.onnx (28 MB)...'
    Invoke-WebRequest -Uri $url -OutFile $partial -UseBasicParsing
    if (-not (Test-ReviaSpeakerModel -Path $partial)) {
        Remove-Item -LiteralPath $partial -Force -ErrorAction SilentlyContinue
        throw 'The speaker model did not match its pinned size and SHA-256; nothing was installed.'
    }
    Move-Item -LiteralPath $partial -Destination $modelPath -Force
    Write-Host 'Verified: wespeaker_en_voxceleb_CAM++.onnx'
}

& $pythonPath -c "import sherpa_onnx; c = sherpa_onnx.SpeakerEmbeddingExtractorConfig(model=r'$modelPath', num_threads=1, provider='cpu'); assert c.validate(); e = sherpa_onnx.SpeakerEmbeddingExtractor(c); print('speaker-id ready; embedding dimension', e.dim)"
if ($LASTEXITCODE -ne 0) {
    throw 'The speaker-id runtime did not pass its model check.'
}

$settingsPath = Join-Path $repoRoot 'Config\settings.json'
if (Test-Path -LiteralPath $settingsPath -PathType Leaf) {
    $settings = Get-Content -LiteralPath $settingsPath -Raw | ConvertFrom-Json
    if ($null -eq $settings.speechRecognition) {
        $settings | Add-Member -NotePropertyName 'speechRecognition' -NotePropertyValue ([pscustomobject]@{}) -Force
    }
    foreach ($pair in @(
        @{ Name = 'speakerIdentificationEnabled'; Value = $true },
        @{ Name = 'speakerServiceScript'; Value = 'Tools/speaker_id_service.py' },
        @{ Name = 'speakerPythonExecutable'; Value = 'ThirdParty/SpeakerId/.venv/Scripts/python.exe' },
        @{ Name = 'speakerPort'; Value = 8098 },
        @{ Name = 'speakerModelPath'; Value = 'Models/wespeaker_en_voxceleb_CAM++.onnx' })) {
        $settings.speechRecognition | Add-Member -NotePropertyName $pair.Name -NotePropertyValue $pair.Value -Force
    }
    Save-ReviaJsonFile -Path $settingsPath -Value $settings
    Write-Host 'Config/settings.json: speechRecognition.speakerIdentificationEnabled = true.'
} else {
    Write-Warning "Config/settings.json was not found; set speechRecognition.speakerIdentificationEnabled to true yourself."
}

Write-Host "Speaker identification is ready: $pythonPath"
Write-Host 'Nobody is recognised until they ask: say "Revia, remember my voice, I''m <name>" a few times, or use /voice enroll <name> after speaking. /voice forget <name> removes it.'
