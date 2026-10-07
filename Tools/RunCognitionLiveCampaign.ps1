param(
    [Parameter(Mandatory = $true)][string]$BuildDirectory,
    [Parameter(Mandatory = $true)][string]$CampaignId,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [Parameter(Mandatory = $true)][string]$ProfilePath,
    [Parameter(Mandatory = $true)][string]$CorpusPath,
    [Parameter(Mandatory = $true)][string]$LaunchMetadataPath,
    [ValidateRange(1, 65535)][int]$Port = 18766,
    [string]$ExecutablePath,
    [string[]]$SettingsFiles = @(),
    [string]$SourceDirectory,
    [switch]$FixtureOnly
)
$ErrorActionPreference = 'Stop'
$utf8 = New-Object Text.UTF8Encoding($false)
if (-not $SourceDirectory) { $SourceDirectory = Join-Path $PSScriptRoot '..' }
$sourceRoot = [IO.Path]::GetFullPath($SourceDirectory)
$outputPath = [IO.Path]::GetFullPath($OutputDirectory)
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
if (-not $ExecutablePath) { $ExecutablePath = Join-Path $BuildDirectory 'ReviaAnswerQualityLive.exe' }
$ExecutablePath = [IO.Path]::GetFullPath($ExecutablePath)
$ProfilePath = [IO.Path]::GetFullPath($ProfilePath)
$CorpusPath = [IO.Path]::GetFullPath($CorpusPath)
$LaunchMetadataPath = [IO.Path]::GetFullPath($LaunchMetadataPath)
if (Test-Path -LiteralPath $outputPath) { throw 'Campaign output already exists; retained evidence cannot be reused.' }
if ($outputPath.StartsWith($sourceRoot.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'Keep campaign evidence outside the source checkout.' }
if (-not $FixtureOnly -and -not $sourceRoot.Equals([IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..')), [StringComparison]::OrdinalIgnoreCase)) { throw 'A foreign source directory is only allowed for an unqualified fixture.' }
if (-not $FixtureOnly -and -not $ExecutablePath.Equals((Join-Path $BuildDirectory 'ReviaAnswerQualityLive.exe'), [StringComparison]::OrdinalIgnoreCase)) { throw 'Live campaigns must invoke the configured ReviaAnswerQualityLive executable.' }
New-Item -ItemType Directory -Path $outputPath | Out-Null

function Write-Once([string]$Name, [byte[]]$Bytes)
{
    $stream = [IO.File]::Open((Join-Path $outputPath $Name), [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
    try { $stream.Write($Bytes, 0, $Bytes.Length); $stream.Flush($true) }
    finally { $stream.Dispose() }
}

function Write-JsonOnce([string]$Name, $Value)
{
    Write-Once $Name ($utf8.GetBytes((ConvertTo-Json -InputObject $Value -Depth 24) + "`n"))
}

function Get-BytesDigest([byte[]]$Bytes)
{
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($algorithm.ComputeHash($Bytes))).Replace('-', '').ToLowerInvariant() }
    finally { $algorithm.Dispose() }
}

function Get-ValueDigest($Value)
{
    return Get-BytesDigest ($utf8.GetBytes((ConvertTo-Json -InputObject $Value -Depth 24 -Compress)))
}

function Get-FileIdentity([string]$Path)
{
    $file = Get-Item -LiteralPath $Path
    if ($file.PSIsContainer) { throw 'An identity file must be an actual file.' }
    $algorithm = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($file.FullName)
    try { $digest = ([BitConverter]::ToString($algorithm.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
    finally { $stream.Dispose(); $algorithm.Dispose() }
    return [ordered]@{ path = $file.FullName; sha256 = $digest; bytes = $file.Length }
}

function Get-FileIdentities([string[]]$Paths)
{
    $files = @($Paths | ForEach-Object {
        $item = Get-Item -LiteralPath $_
        if ($item.PSIsContainer) { Get-ChildItem -LiteralPath $item.FullName -Recurse -File }
        else { $item }
    } | Sort-Object FullName -Unique)
    return @($files | ForEach-Object { Get-FileIdentity $_.FullName })
}

function Invoke-Git([string[]]$Arguments)
{
    try {
        $ErrorActionPreference = 'Continue'
        $captured = @(& git -C $sourceRoot @Arguments 2>> (Join-Path $outputPath 'git-errors.log'))
        $gitExit = $LASTEXITCODE
    } finally { $ErrorActionPreference = 'Stop' }
    if ($gitExit -ne 0) { throw 'Git source identity capture failed.' }
    return $captured
}

function Get-SourceIdentity([string]$Phase)
{
    $paths = @('Public', 'Private', 'Desktop', 'Tools', 'Tests', 'Config', 'docs', 'CMakeLists.txt', 'CMakePresets.json', '.clang-format', 'AGENTS.md')
    $commit = (Invoke-Git @('rev-parse', 'HEAD')) -join ''
    $scratch = [IO.Path]::GetTempFileName()
    try {
        $null = Invoke-Git (@('diff', '--binary', '--no-ext-diff', '--no-color', 'HEAD', ('--output=' + $scratch), '--') + $paths)
        $patch = [IO.File]::ReadAllBytes($scratch)
    } finally { Remove-Item -LiteralPath $scratch -Force }
    Write-Once ($Phase + '.source.patch') $patch
    $untrackedPaths = @(Invoke-Git (@('-c', 'core.quotepath=false', 'ls-files', '--others', '--exclude-standard', '--') + $paths))
    $untracked = @($untrackedPaths | Sort-Object | ForEach-Object {
        $identity = Get-FileIdentity (Join-Path $sourceRoot $_)
        $identity.path = $_
        $identity
    })
    $identity = [ordered]@{ commit = $commit; sourceDirty = ($patch.Length -gt 0 -or $untracked.Count -gt 0); patchSha256 = Get-BytesDigest $patch; untracked = $untracked }
    $digest = Get-ValueDigest $identity
    $identity['digest'] = $digest
    $identity['root'] = $sourceRoot
    return $identity
}

function Get-ProviderIdentity([string]$Phase)
{
    $launch = Get-Content -LiteralPath $LaunchMetadataPath -Raw | ConvertFrom-Json
    $listeners = @(Get-NetTCPConnection -LocalPort $Port -State Listen | Where-Object { $_.LocalAddress -eq '127.0.0.1' } | Select-Object -ExpandProperty OwningProcess -Unique)
    if ($listeners.Count -ne 1) { throw 'A unique owned loopback listening process is required.' }
    $processId = [int]$listeners[0]
    $ancestor = $processId
    $owned = $false
    for ($depth = 0; $depth -lt 16 -and $ancestor -gt 0; ++$depth) {
        if ($ancestor -eq [int]$launch.pid) { $owned = $true; break }
        $parent = Get-CimInstance Win32_Process -Filter "ProcessId=$ancestor"
        if (-not $parent) { break }
        $ancestor = [int]$parent.ParentProcessId
    }
    if (-not $owned) { throw 'The listener is not the process recorded by the owned launch receipt.' }
    $process = Get-Process -Id $processId
    $launchProcess = Get-Process -Id ([int]$launch.pid)
    $launchStarted = if ($launch.started -is [DateTime]) { $launch.started.ToUniversalTime() } else { [DateTimeOffset]::Parse([string]$launch.started).UtcDateTime }
    if ([Math]::Abs(($launchProcess.StartTime.ToUniversalTime() - $launchStarted).TotalSeconds) -gt 5) { throw 'Owned launch PID was reused by a different process.' }
    $baseUri = "http://127.0.0.1:$Port"
    $inventory = Invoke-RestMethod -Uri ($baseUri + '/v1/models') -TimeoutSec 10
    $properties = Invoke-RestMethod -Uri ($baseUri + '/props') -TimeoutSec 10
    Write-JsonOnce ($Phase + '.models.json') $inventory
    Write-JsonOnce ($Phase + '.props.json') $properties
    $models = @($inventory.data)
    if ($models.Count -ne 1 -or [string]::IsNullOrWhiteSpace([string]$models[0].id)) { throw 'Exactly one observed model ID is required.' }
    # Full receipts retain response timestamps; only data[*].created is excluded from model identity.
    $inventoryIdentity = ConvertTo-Json -InputObject $inventory -Depth 24 | ConvertFrom-Json
    foreach ($entry in @($inventoryIdentity.data)) {
        if (@($entry.PSObject.Properties.Name) -ccontains 'created') { $entry.PSObject.Properties.Remove('created') }
    }
    $modelPath = [IO.Path]::GetFullPath([string]$properties.model_path)
    if (-not $modelPath.Equals([IO.Path]::GetFullPath([string]$launch.model), [StringComparison]::OrdinalIgnoreCase)) { throw 'Observed model path differs from the owned launch receipt.' }
    $wrapperDirectory = [IO.Path]::GetDirectoryName([IO.Path]::GetFullPath([string]$launch.server))
    $processDirectory = [IO.Path]::GetDirectoryName($process.Path)
    $modules = @($process.Modules | Where-Object {
        [IO.Path]::GetDirectoryName($_.FileName).Equals($wrapperDirectory, [StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetDirectoryName($_.FileName).Equals($processDirectory, [StringComparison]::OrdinalIgnoreCase)
    } | Select-Object -ExpandProperty FileName -Unique | Sort-Object)
    $identity = [ordered]@{
        launch = Get-FileIdentity $LaunchMetadataPath; wrapper = Get-FileIdentity ([string]$launch.server)
        resolvedServerExecutable = Get-FileIdentity $process.Path; model = Get-FileIdentity $modelPath
        loadedProviderModules = @(Get-FileIdentities $modules); observedModelId = [string]$models[0].id
        inventorySha256 = Get-ValueDigest $inventoryIdentity; propertiesSha256 = Get-ValueDigest $properties
        listeningProcessId = $processId; listeningProcessStarted = $process.StartTime.ToUniversalTime().ToString('o')
        launchArguments = @($launch.arguments); providerIdentityVerified = $false; backendSeedVerified = $false
    }
    $identity['digest'] = Get-ValueDigest $identity
    return $identity
}

function Get-Receipt([string]$Phase)
{
    $source = Get-SourceIdentity $Phase
    $cachePath = Join-Path $BuildDirectory 'CMakeCache.txt'
    $configured = @(Get-Content -LiteralPath $cachePath | Where-Object { $_.StartsWith('CMAKE_HOME_DIRECTORY:INTERNAL=') })
    if ($configured.Count -ne 1 -or -not ([IO.Path]::GetFullPath($configured[0].Substring('CMAKE_HOME_DIRECTORY:INTERNAL='.Length))).Equals($sourceRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Configured build source differs from the captured source checkout.' }
    $buildFiles = @($cachePath, $ExecutablePath)
    foreach ($relative in 'build.ninja', 'compile_commands.json', 'CMakeFiles/rules.ninja') {
        $path = Join-Path $BuildDirectory $relative
        if (Test-Path -LiteralPath $path -PathType Leaf) { $buildFiles += $path }
    }
    $buildArtifacts = Get-FileIdentities $buildFiles
    $build = [ordered]@{ directory = $BuildDirectory; executable = Get-FileIdentity $ExecutablePath; artifacts = $buildArtifacts; digest = Get-ValueDigest $buildArtifacts; coverage = 'Configured cache/build rules and actual executable; complete transitive build environment is not claimed.' }
    $profile = Get-FileIdentity $ProfilePath
    $corpus = Get-FileIdentity $CorpusPath
    $population = Get-Content -LiteralPath $CorpusPath -Raw | ConvertFrom-Json
    $seeds = @($population.seeds)
    if ($seeds.Count -eq 0 -or @($seeds | Sort-Object -Unique).Count -ne $seeds.Count) { throw 'Corpus seeds must be nonempty and unique.' }
    $settingsPaths = $SettingsFiles
    if ($settingsPaths.Count -eq 0) { $settingsPaths = @(Join-Path $sourceRoot 'Config') }
    $settings = @(Get-FileIdentities $settingsPaths)
    $provider = Get-ProviderIdentity $Phase
    return [ordered]@{
        schemaVersion = 1; campaignId = $CampaignId; phase = $Phase; capturedAt = [DateTime]::UtcNow.ToString('o')
        source = $source; build = $build; provider = $provider; profile = $profile; corpus = $corpus
        settings = $settings; settingsDigest = Get-ValueDigest ([ordered]@{ profile = $profile; files = $settings })
        seeds = $seeds; uniqueCases = @($population.cases).Count; expectedRuns = @($population.cases).Count * $seeds.Count
        fixtureOnly = [bool]$FixtureOnly; liveQualified = $false; providerIdentityVerified = $false; backendSeedVerified = $false
        independentSemanticReview = 'pending'; independentPersonalityReview = 'pending'
        timingBoundary = 'owned-wrapper-before-receipt-to-after-receipt; evaluator-per-case timing retained separately'
    }
}

$runnerExit = 2
$sourceBuildVerified = $false
try {
    $admissionSource = Get-SourceIdentity 'admission'
    Write-JsonOnce 'admission.source.json' $admissionSource
    if (-not $FixtureOnly) {
        $cachePath = Join-Path $BuildDirectory 'CMakeCache.txt'
        $configured = @(Get-Content -LiteralPath $cachePath | Where-Object { $_.StartsWith('CMAKE_HOME_DIRECTORY:INTERNAL=') })
        if ($configured.Count -ne 1 -or -not ([IO.Path]::GetFullPath($configured[0].Substring('CMAKE_HOME_DIRECTORY:INTERNAL='.Length))).Equals($sourceRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Configured build source differs from this Revia checkout.' }
        $priorPath = $env:PATH
        $compiler = @(Get-Content -LiteralPath $cachePath | Where-Object { $_ -match '^CMAKE_CXX_COMPILER:[^=]+=' })
        if ($compiler.Count -eq 1) { $env:PATH = [IO.Path]::GetDirectoryName(($compiler[0] -split '=', 2)[1]) + [IO.Path]::PathSeparator + $env:PATH }
        try {
            $ErrorActionPreference = 'Continue'
            & cmake -S $sourceRoot -B $BuildDirectory *> (Join-Path $outputPath 'configure.log')
            if ($LASTEXITCODE -ne 0) { throw 'Configuring the admitted Revia source failed.' }
            & cmake --build $BuildDirectory --target ReviaAnswerQualityLive --parallel 4 *> (Join-Path $outputPath 'build.log')
            if ($LASTEXITCODE -ne 0) { throw 'Building the admitted live cognition target failed.' }
        } finally { $env:PATH = $priorPath; $ErrorActionPreference = 'Stop' }
        $sourceBuildVerified = $true
    }
    $before = Get-Receipt 'before'
    $before['sourceBuildVerified'] = $sourceBuildVerified
    if ($admissionSource.digest -ne $before.source.digest) { throw 'Source changed during configure/build; admitted source cannot be assigned to the executable.' }
    Write-JsonOnce 'before.json' $before
    $hardware = [ordered]@{ machine = [Environment]::MachineName; os = [Environment]::OSVersion.VersionString; cpu = $env:PROCESSOR_IDENTIFIER; logicalProcessors = [Environment]::ProcessorCount }
    try {
        $hardware['memoryBytes'] = (Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory
        $hardware['graphics'] = @(Get-CimInstance Win32_VideoController | Sort-Object Name | ForEach-Object {
            [ordered]@{ name = $_.Name; driver = $_.DriverVersion; reportedMemoryBytes = $_.AdapterRAM }
        })
    } catch { $hardware['extendedInventory'] = 'unavailable' }
    $manifest = [ordered]@{
        schemaVersion = 1; campaignId = $CampaignId; commit = $before.source.commit; sourceDigest = $before.source.digest; sourceDirty = $before.source.sourceDirty
        buildDigest = $before.build.digest; providerDigest = $before.provider.digest; providerAvailable = $true
        settingsDigest = $before.settingsDigest; fixtureDigest = $before.corpus.sha256; oracleVersion = 'json-exact-v1'
        hardware = ConvertTo-Json -InputObject $hardware -Compress; seed = [UInt64]0
        timingBoundary = 'existing-conversation-evaluator-per-case-wall-time'
    }
    Write-JsonOnce 'base-manifest.json' $manifest
    $reportPath = Join-Path $outputPath 'cognition.json'
    $failure = ''
    $timer = [Diagnostics.Stopwatch]::StartNew()
    try {
        $ErrorActionPreference = 'Continue'
        & $ExecutablePath $ProfilePath $CorpusPath $Port $reportPath cognition (Join-Path $outputPath 'base-manifest.json') *> (Join-Path $outputPath 'runner.log')
        $runnerExit = $LASTEXITCODE
    } catch { $failure = $_.Exception.Message }
    finally { $ErrorActionPreference = 'Stop'; $timer.Stop() }
    $after = Get-Receipt 'after'
    $after['sourceBuildVerified'] = $sourceBuildVerified
    Write-JsonOnce 'after.json' $after
    $mismatches = @()
    foreach ($field in 'source', 'build', 'provider') { if ($before[$field].digest -ne $after[$field].digest) { $mismatches += $field } }
    foreach ($field in 'profile', 'corpus') { if ($before[$field].sha256 -ne $after[$field].sha256) { $mismatches += $field } }
    if ($before.settingsDigest -ne $after.settingsDigest) { $mismatches += 'settings' }
    $aggregate = $null
    if (Test-Path -LiteralPath $reportPath -PathType Leaf) { $aggregate = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json }
    if ($null -eq $aggregate -or $aggregate.liveQualified -ne $false) { $failure = 'CLI aggregate missing or claimed live qualification.' }
    $artifacts = Get-FileIdentities @(Get-ChildItem -LiteralPath $outputPath -Recurse -File | Where-Object { $_.Name -ne 'git-errors.log' -and -not $_.Name.EndsWith('.progress.json') } | Select-Object -ExpandProperty FullName)
    $results = [ordered]@{
        schemaVersion = 1; campaignId = $CampaignId; runnerExitCode = $runnerExit; failure = $failure
        provenanceStable = ($mismatches.Count -eq 0); sourceBuildVerified = $sourceBuildVerified; mismatches = $mismatches; elapsedMilliseconds = $timer.ElapsedMilliseconds
        qualification = $(if ($FixtureOnly) { 'fixture-only-unqualified' } else { 'pending-external-verification-and-human-review' })
        liveQualified = $false; providerIdentityVerified = $false; backendSeedVerified = $false
        independentSemanticReview = 'pending'; independentPersonalityReview = 'pending'; aggregate = $aggregate; artifacts = $artifacts
    }
    Write-JsonOnce 'results.json' $results
    if ($runnerExit -ne 0 -or $failure -or $mismatches.Count -gt 0) { exit 2 }
    Write-Output "Retained owned campaign receipts at $outputPath; qualification and independent reviews remain pending."
    exit 0
} catch {
    if (-not (Test-Path -LiteralPath (Join-Path $outputPath 'results.json'))) {
        Write-JsonOnce 'results.json' ([ordered]@{ schemaVersion = 1; campaignId = $CampaignId; runnerExitCode = $runnerExit; failure = $_.Exception.Message; provenanceStable = $false; liveQualified = $false; providerIdentityVerified = $false; backendSeedVerified = $false; independentSemanticReview = 'pending'; independentPersonalityReview = 'pending' })
    }
    Write-Error 'Cognition campaign could not retain matching owned provenance; inspect the retained receipts.' -ErrorAction Continue
    exit 2
}
