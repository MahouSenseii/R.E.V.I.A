param(
    [ValidateSet('Minimal', 'Standard', 'Full')]
    [string]$Profile = 'Full',
    [switch]$SkipModels,
    [switch]$SkipVoice,
    [switch]$SkipBuild,
    [switch]$SkipTests
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$logDirectory = Join-Path $repoRoot 'Logs'
New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null
$transcriptPath = Join-Path $logDirectory 'setup.log'

try {
    Start-Transcript -LiteralPath $transcriptPath -Append | Out-Null
    Write-Host '============================================================'
    Write-Host "R.E.V.I.A setup — $Profile profile"
    Write-Host 'Existing models and user data are verified and reused.'
    Write-Host '============================================================'

    if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
        throw 'The automatic full setup currently supports Windows only.'
    }
    if ($PSVersionTable.PSVersion.Major -lt 5) {
        throw 'Windows PowerShell 5.1 or newer is required.'
    }

    Push-Location $repoRoot
    try {
        Write-Host "`n[1/6] Installing or verifying the Qt/MinGW build toolchain..."
        & (Join-Path $PSScriptRoot 'InstallQt.ps1')
        if ($LASTEXITCODE -ne 0) { throw 'Qt/toolchain setup failed.' }

        Write-Host "`n[2/6] Installing or verifying the hardware-matched llama.cpp runtime..."
        & (Join-Path $PSScriptRoot 'InstallLlamaCpp.ps1')
        if ($LASTEXITCODE -ne 0) { throw 'llama.cpp setup failed.' }

        Write-Host "`n[3/6] Installing or verifying Whisper speech recognition..."
        & (Join-Path $PSScriptRoot 'InstallWhisper.ps1')
        if ($LASTEXITCODE -ne 0) { throw 'Whisper setup failed.' }

        if (-not $SkipVoice) {
            Write-Host "`n[4/6] Installing or verifying the project-local Qwen3-TTS environment..."
            & (Join-Path $PSScriptRoot 'InstallQwenTTS.ps1')
            if ($LASTEXITCODE -ne 0) { throw 'Qwen3-TTS setup failed.' }
        } else {
            Write-Host "`n[4/6] Voice installation skipped by request. Windows SAPI remains the fallback."
        }

        if (-not $SkipModels) {
            Write-Host "`n[5/6] Downloading or verifying the pinned $Profile model profile..."
            & (Join-Path $PSScriptRoot 'DownloadRuntimeModels.ps1') -Profile $Profile
            if ($LASTEXITCODE -ne 0) { throw 'Model setup failed.' }
        } else {
            Write-Host "`n[5/6] Model downloads skipped by request. Existing artifacts will be checked."
        }

        if (-not $SkipBuild) {
            Write-Host "`n[6/6] Building Revia and running regression tests..."
            & (Join-Path $PSScriptRoot 'Build.ps1') -SkipTests:$SkipTests
            if ($LASTEXITCODE -ne 0) { throw 'Build or regression tests failed.' }
        } else {
            Write-Host "`n[6/6] Build skipped by request."
        }

        Write-Host "`nRunning the final health check..."
        & (Join-Path $PSScriptRoot 'HealthCheck.ps1') -Profile $Profile -SkipVoice:$SkipVoice
        if ($LASTEXITCODE -ne 0) { throw 'The final health check found blocking problems.' }

        Write-Host "`nRevia setup completed successfully."
        $desktop = Join-Path $repoRoot 'build\debug\ReviaDesktop.exe'
        if (Test-Path -LiteralPath $desktop -PathType Leaf) {
            Write-Host "Start Revia with: $desktop"
        }
    }
    finally {
        Pop-Location
    }
}
catch {
    Write-Error $_
    Write-Host "Setup details were written to $transcriptPath"
    exit 1
}
finally {
    try { Stop-Transcript | Out-Null } catch { }
}
