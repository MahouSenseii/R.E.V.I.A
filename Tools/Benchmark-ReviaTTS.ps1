[CmdletBinding()]
param(
    [ValidateSet('baseline', 'optimized', 'comparison')]
    [string]$Label = 'comparison',
    [string[]]$Devices = @('cuda:0'),
    [string]$SettingsPath = 'Config/settings.json',
    [string]$VoiceStorePath = 'RuntimeData/Voices/voices.json',
    [int]$BasePort = 8190,
    [switch]$SkipModelPrepare,
    [switch]$Quick,
    [ValidateSet('', 'complete', 'simulated-stream')]
    [string]$InputModeOverride = '',
    [ValidateSet('', 'adaptive', 'auto', 'eager', 'sdpa', 'flash_attention_2')]
    [string]$AttentionBackendOverride = ''
)

$ErrorActionPreference = 'Stop'

function Resolve-ReviaPath([string]$Value) {
    if ([IO.Path]::IsPathRooted($Value)) { return [IO.Path]::GetFullPath($Value) }
    return [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\$Value"))
}

function Get-WavDurationSeconds([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    try {
        $reader = [IO.BinaryReader]::new($stream)
        if ([Text.Encoding]::ASCII.GetString($reader.ReadBytes(4)) -ne 'RIFF') { return 0.0 }
        $null = $reader.ReadUInt32()
        if ([Text.Encoding]::ASCII.GetString($reader.ReadBytes(4)) -ne 'WAVE') { return 0.0 }
        $byteRate = 0
        $dataBytes = 0
        while ($stream.Position + 8 -le $stream.Length) {
            $chunk = [Text.Encoding]::ASCII.GetString($reader.ReadBytes(4))
            $length = $reader.ReadUInt32()
            if ($chunk -eq 'fmt ') {
                $null = $reader.ReadUInt16()
                $null = $reader.ReadUInt16()
                $null = $reader.ReadUInt32()
                $byteRate = $reader.ReadUInt32()
                $stream.Position += [Math]::Max(0, $length - 12)
            } elseif ($chunk -eq 'data') {
                $dataBytes = $length
                break
            } else {
                $stream.Position += $length
            }
            if (($length % 2) -ne 0) { $stream.Position++ }
        }
        if ($byteRate -le 0) { return 0.0 }
        return [double]$dataBytes / [double]$byteRate
    } finally {
        $stream.Dispose()
    }
}

function Invoke-QwenJson(
    [string]$Method,
    [string]$Uri,
    [string]$Token,
    [object]$Body = $null,
    [int]$TimeoutSeconds = 900) {
    $parameters = @{
        Method = $Method
        Uri = $Uri
        Headers = @{ Authorization = "Bearer $Token" }
        TimeoutSec = $TimeoutSeconds
    }
    if ($null -ne $Body) {
        $parameters.ContentType = 'application/json'
        $parameters.Body = ($Body | ConvertTo-Json -Depth 8 -Compress)
    }
    return Invoke-RestMethod @parameters
}

function Get-GpuSnapshot([string]$Device) {
    if ($Device -notmatch '^cuda:(\d+)$') { return $null }
    $index = [int]$Matches[1]
    $tool = Get-Command nvidia-smi.exe -ErrorAction SilentlyContinue
    if ($null -eq $tool) { return $null }
    $rows = & $tool.Source --query-gpu=index,name,memory.used,utilization.gpu `
        --format=csv,noheader,nounits 2>$null
    foreach ($row in $rows) {
        $parts = @($row -split ',' | ForEach-Object { $_.Trim() })
        if ($parts.Count -ge 4 -and [int]$parts[0] -eq $index) {
            return [pscustomobject]@{
                name = $parts[1]
                residentMemoryMiB = [int]$parts[2]
                sampledUtilizationPercent = [int]$parts[3]
            }
        }
    }
    return $null
}

$settingsFile = Resolve-ReviaPath $SettingsPath
$voiceFile = Resolve-ReviaPath $VoiceStorePath
$settings = Get-Content -LiteralPath $settingsFile -Raw | ConvertFrom-Json
$voiceStore = Get-Content -LiteralPath $voiceFile -Raw | ConvertFrom-Json
$preset = $voiceStore.presets | Select-Object -First 1
if ($null -eq $preset) { throw "No voice preset exists in $voiceFile." }

$python = Resolve-ReviaPath $settings.speech.pythonExecutable
$service = Resolve-ReviaPath $settings.speech.qwenServiceScript
$reference = Resolve-ReviaPath $preset.referenceAudioPath
$benchmarkRoot = Resolve-ReviaPath 'RuntimeData/Benchmarks'
$audioRoot = Join-Path $benchmarkRoot 'TTS-Audio'
New-Item -ItemType Directory -Force -Path $benchmarkRoot, $audioRoot | Out-Null

$phrases = @(
    'Yeah, I found it.',
    'Okay, I think I found the issue.',
    'The pointer becomes invalid before that callback can safely run.',
    'I found the problem: the callback keeps a borrowed pointer after its owner has already released it.',
    'I found the problem. The callback keeps a borrowed pointer after its owner has released it, so the later access races with destruction. Add ownership tracing, run AddressSanitizer and ThreadSanitizer separately, and reproduce it under repeated scheduling pressure.'
)
if ($Quick) { $phrases = @($phrases[0], $phrases[2]) }

$machine = $env:COMPUTERNAME
$startedUtc = [DateTime]::UtcNow.ToString('o')
$all = [Collections.Generic.List[object]]::new()
$deviceIndex = 0
foreach ($device in $Devices) {
    $port = $BasePort + $deviceIndex
    $deviceIndex++
    $token = [guid]::NewGuid().ToString('N')
    $safeDevice = $device -replace '[^A-Za-z0-9_-]', '_'
    $stdout = Join-Path $benchmarkRoot "qwen-$Label-$safeDevice.stdout.log"
    $stderr = Join-Path $benchmarkRoot "qwen-$Label-$safeDevice.stderr.log"
    $arguments = @(
        $service, '--host', '127.0.0.1', '--port', [string]$port,
        '--token', $token, '--device', $device,
        '--minimum-free-vram-mib', '0',
        '--cpu-threads', [string]([Math]::Max(1, [int]$settings.speech.qwenCpuThreads)),
        '--max-audio-mib', [string]([int]$settings.speech.qwenMaxBufferedAudioMiB),
        '--design-model', [string]$settings.speech.qwenVoiceDesignModel,
        '--clone-model', [string]$settings.speech.qwenCloneModel
    )
    $inputMode = if ($InputModeOverride) { $InputModeOverride } elseif ($Label -eq 'baseline') {
        'simulated-stream'
    } else {
        if ($settings.speech.qwenInputMode) { [string]$settings.speech.qwenInputMode } else { 'complete' }
    }
    $attention = if ($AttentionBackendOverride) { $AttentionBackendOverride } elseif ($settings.speech.qwenAttentionBackend) {
        [string]$settings.speech.qwenAttentionBackend
    } else { 'auto' }
    $arguments += @('--input-mode', $inputMode, '--attention-backend', $attention)
    $process = Start-Process -FilePath $python -ArgumentList $arguments -PassThru `
        -WindowStyle Hidden -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    try {
        $baseUri = "http://127.0.0.1:$port"
        $deadline = [DateTime]::UtcNow.AddSeconds(90)
        $health = $null
        while ([DateTime]::UtcNow -lt $deadline -and !$process.HasExited) {
            try {
                $health = Invoke-QwenJson GET "$baseUri/health" $token $null 2
                break
            } catch {
                Start-Sleep -Milliseconds 250
            }
        }
        if ($null -eq $health) { throw "Qwen worker on $device did not become healthy. See $stderr." }

        $prepare = $null
        if (!$SkipModelPrepare) {
            $prepareWatch = [Diagnostics.Stopwatch]::StartNew()
            $prepareVoice = $Label -ne 'baseline' -and
                ($settings.speech.qwenPrecomputeVoicePrompt -ne $false)
            $prepareEndpoint = if ($prepareVoice) { '/prepare-voice' } else { '/prepare' }
            $prepareBody = if ($prepareVoice) {
                @{ reference_audio = $reference; reference_text = [string]$preset.referenceText }
            } else { @{ model = 'clone' } }
            $prepare = Invoke-QwenJson POST "$baseUri$prepareEndpoint" $token $prepareBody
            $prepareWatch.Stop()
            $gpu = Get-GpuSnapshot ([string]$prepare.device)
            $all.Add([pscustomobject]@{
                kind = 'prepare'; device = $prepare.device; deviceName = $prepare.device_name
                dtype = $prepare.dtype; cudaMathMode = $prepare.cuda_math_mode
                characters = 0; text = ''
                serverMilliseconds = [double]$prepare.elapsed_ms
                wallMilliseconds = $prepareWatch.Elapsed.TotalMilliseconds
                audioSeconds = 0.0; rtf = 0.0; charactersPerSecond = 0.0
                clonePromptCached = [bool]$prepare.clone_prompt_cached
                gpuResidentMemoryMiB = if ($gpu) { $gpu.residentMemoryMiB } else { $null }
                gpuSampledUtilizationPercent = if ($gpu) { $gpu.sampledUtilizationPercent } else { $null }
                succeeded = [bool]$prepare.succeeded
            })
        }

        $phraseIndex = 0
        foreach ($text in $phrases) {
            $phraseIndex++
            $output = Join-Path $audioRoot "$Label-$safeDevice-$phraseIndex.wav"
            $body = @{
                text = $text; language = [string]$preset.language
                reference_audio = $reference; reference_text = [string]$preset.referenceText
                output_path = $output
            }
            $watch = [Diagnostics.Stopwatch]::StartNew()
            $result = Invoke-QwenJson POST "$baseUri/v1/audio/speech" $token $body
            $watch.Stop()
            $duration = if (Test-Path -LiteralPath $output) {
                Get-WavDurationSeconds $output
            } else { 0.0 }
            $seconds = [Math]::Max(0.001, [double]$result.elapsed_ms / 1000.0)
            $gpu = Get-GpuSnapshot ([string]$result.device)
            $all.Add([pscustomobject]@{
                kind = 'synthesis'; device = $result.device; deviceName = $result.device_name
                dtype = $result.dtype; cudaMathMode = $result.cuda_math_mode
                characters = $text.Length; text = $text
                serverMilliseconds = [double]$result.elapsed_ms
                wallMilliseconds = $watch.Elapsed.TotalMilliseconds
                audioSeconds = $duration
                rtf = if ($duration -gt 0) { $seconds / $duration } else { 0.0 }
                charactersPerSecond = $text.Length / $seconds
                clonePromptCached = if ($null -ne $result.clone_prompt_cached) {
                    [bool]$result.clone_prompt_cached
                } else { $phraseIndex -gt 1 }
                gpuResidentMemoryMiB = if ($gpu) { $gpu.residentMemoryMiB } else { $null }
                gpuSampledUtilizationPercent = if ($gpu) { $gpu.sampledUtilizationPercent } else { $null }
                succeeded = [bool]$result.succeeded
            })
        }
    } finally {
        if (!$process.HasExited) { Stop-Process -Id $process.Id -Force }
        $process.WaitForExit()
    }
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$outputJson = Join-Path $benchmarkRoot "TTS_$machine-$Label-$stamp.json"
$document = [ordered]@{
    schemaVersion = 1
    label = $Label
    machine = $machine
    startedUtc = $startedUtc
    finishedUtc = [DateTime]::UtcNow.ToString('o')
    model = [string]$settings.speech.qwenCloneModel
    attentionBackend = if ($AttentionBackendOverride) { $AttentionBackendOverride } elseif ($settings.speech.qwenAttentionBackend) {
        [string]$settings.speech.qwenAttentionBackend
    } else { 'auto' }
    compileMode = 'off'
    inputMode = if ($InputModeOverride) { $InputModeOverride } elseif ($Label -eq 'baseline') {
        'simulated-stream'
    } else { [string]$settings.speech.qwenInputMode }
    trueIncrementalAudioStreaming = $false
    note = 'firstAudio equals completed-phrase generation in this batch endpoint benchmark'
    samples = $all
}
$document | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $outputJson -Encoding utf8
Write-Output "TTS benchmark written to $outputJson"
$all | Format-Table kind,device,dtype,cudaMathMode,characters,serverMilliseconds,audioSeconds,rtf,charactersPerSecond,succeeded -AutoSize
