param(
    [ValidateSet('Minimal', 'Standard', 'Full')]
    [string]$Profile = 'Full',
    [switch]$SkipVoice,
    [switch]$SkipHashes
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$failures = [System.Collections.Generic.List[string]]::new()
$warnings = [System.Collections.Generic.List[string]]::new()

function Write-ReviaCheck {
    param([string]$Name, [bool]$Passed, [string]$Detail)
    $marker = if ($Passed) { '[PASS]' } else { '[FAIL]' }
    Write-Host "$marker $Name — $Detail"
    if (-not $Passed) { $failures.Add("$Name`: $Detail") }
}

function Resolve-ReviaPath {
    param([string]$Path)
    if ([IO.Path]::IsPathRooted($Path)) { return $Path }
    return Join-Path $repoRoot $Path
}

Push-Location $repoRoot
try {
    Write-Host "R.E.V.I.A health check — $Profile profile"

    $settingsPath = Join-Path $repoRoot 'Config\settings.json'
    try {
        $settings = Get-Content -LiteralPath $settingsPath -Raw | ConvertFrom-Json
        Write-ReviaCheck 'Configuration' $true 'Config/settings.json is valid JSON.'
    } catch {
        Write-ReviaCheck 'Configuration' $false $_.Exception.Message
        $settings = $null
    }

    $manifestPath = Join-Path $repoRoot 'Config\model_manifest.json'
    try {
        $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
        $ids = @($manifest.profiles.$Profile)
        Write-ReviaCheck 'Model manifest' ($ids.Count -gt 0) "$($ids.Count) artifacts selected."
        foreach ($id in $ids) {
            $artifact = @($manifest.artifacts | Where-Object { $_.id -eq $id })
            if ($artifact.Count -ne 1) {
                Write-ReviaCheck "Model $id" $false 'Manifest entry is missing or duplicated.'
                continue
            }
            $path = Join-Path $repoRoot ('Models\' + $artifact[0].file)
            if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
                Write-ReviaCheck "Model $id" $false "Missing $($artifact[0].file)."
                continue
            }
            $item = Get-Item -LiteralPath $path
            if ($item.Length -ne [long]$artifact[0].bytes) {
                Write-ReviaCheck "Model $id" $false "Size is $($item.Length), expected $($artifact[0].bytes)."
                continue
            }
            if (-not $SkipHashes) {
                $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
                if ($hash -ne ([string]$artifact[0].sha256).ToLowerInvariant()) {
                    Write-ReviaCheck "Model $id" $false 'SHA-256 does not match the pinned artifact.'
                    continue
                }
            }
            Write-ReviaCheck "Model $id" $true "$($artifact[0].role) is verified."
        }
    } catch {
        Write-ReviaCheck 'Model manifest' $false $_.Exception.Message
    }

    if ($null -ne $settings) {
        $ports = @(
            [int]$settings.llm.port,
            [int]$settings.embedding.port,
            [int]$settings.speech.qwenPort,
            [int]$settings.speechRecognition.serverPort,
            [int]$settings.intelligence.fast.port,
            [int]$settings.intelligence.expert.port
        )
        Write-ReviaCheck 'Service ports' (($ports | Sort-Object -Unique).Count -eq $ports.Count) `
            ($ports -join ', ')

        $portablePaths = @(
            $settings.llm.serverExecutable,
            $settings.llm.modelPath,
            $settings.embedding.modelPath,
            $settings.speechRecognition.modelPath,
            $settings.intelligence.fast.modelPath,
            $settings.intelligence.expert.modelPath
        )
        $machineSpecific = @($portablePaths | Where-Object { [IO.Path]::IsPathRooted([string]$_) })
        Write-ReviaCheck 'Portable paths' ($machineSpecific.Count -eq 0) `
            $(if ($machineSpecific.Count -eq 0) { 'All configured runtime paths are repository-relative.' } else { $machineSpecific -join ', ' })
    }

    $llama = Join-Path $repoRoot 'ThirdParty\llama.cpp\llama-server.exe'
    Write-ReviaCheck 'llama.cpp runtime' (Test-Path -LiteralPath $llama -PathType Leaf) $llama
    if (Test-Path -LiteralPath $llama -PathType Leaf) {
        $deviceOutput = & $llama --list-devices 2>&1 | Out-String
        Write-ReviaCheck 'Hardware inventory' ($LASTEXITCODE -eq 0) $deviceOutput.Trim()
    }

    $whisper = Join-Path $repoRoot 'ThirdParty\whisper\whisper-server.exe'
    Write-ReviaCheck 'Whisper runtime' (Test-Path -LiteralPath $whisper -PathType Leaf) $whisper

    if (-not $SkipVoice -and $null -ne $settings) {
        $python = Resolve-ReviaPath ([string]$settings.speech.pythonExecutable)
        $voiceScript = Resolve-ReviaPath ([string]$settings.speech.qwenServiceScript)
        Write-ReviaCheck 'Qwen3-TTS Python' (Test-Path -LiteralPath $python -PathType Leaf) $python
        Write-ReviaCheck 'Qwen3-TTS service' (Test-Path -LiteralPath $voiceScript -PathType Leaf) $voiceScript
    }

    $desktop = Join-Path $repoRoot 'build\debug\ReviaDesktop.exe'
    Write-ReviaCheck 'Desktop executable' (Test-Path -LiteralPath $desktop -PathType Leaf) $desktop

    $dataBase = if (Test-Path -LiteralPath $desktop -PathType Leaf) {
        Split-Path -Parent $desktop
    } else { $repoRoot }
    $memoryRoot = Join-Path $dataBase 'Memory'
    $runtimeRoot = Join-Path $dataBase 'RuntimeData'
    Write-ReviaCheck 'Persistent data roots' `
        ((Test-Path -LiteralPath $memoryRoot -PathType Container) -and
         (Test-Path -LiteralPath $runtimeRoot -PathType Container)) `
        'Memory and RuntimeData are present and were not modified by this check.'

    if ($failures.Count -gt 0) {
        Write-Host "`nHealth check failed with $($failures.Count) blocking problem(s)."
        exit 1
    }
    Write-Host "`nHealth check passed. Live model/TTS latency still requires an actual Revia session."
}
finally {
    Pop-Location
}
