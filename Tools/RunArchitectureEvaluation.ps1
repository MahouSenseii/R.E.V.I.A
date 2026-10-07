param(
    [Parameter(Mandatory = $true)][string]$Preset,
    [Parameter(Mandatory = $true)][string]$CampaignId,
    [Parameter(Mandatory = $true)][string]$ChangeId,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [string]$BuildDirectory,
    [string]$RegistryPath = (Join-Path $PSScriptRoot '../Config/Evaluation/architecture_tests.json'),
    [string[]]$ProviderFiles = @(),
    [string[]]$SettingsFiles = @(),
    [string[]]$FixtureFiles = @(),
    [string]$OracleVersion = 'architecture-fixture-v1',
    [UInt64]$Seed = 0,
    [ValidateSet('ctest-process-start-to-exit')][string]$TimingBoundary = 'ctest-process-start-to-exit'
)
$ErrorActionPreference = 'Stop'
$sourceRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$outputPath = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $outputPath) { throw 'Campaign output already exists; choose a new directory to preserve immutable evidence.' }
New-Item -ItemType Directory -Path $outputPath | Out-Null
$utf8 = New-Object Text.UTF8Encoding($false)

function Write-Once([string]$Name, [string]$Text)
{
    $path = Join-Path $outputPath $Name
    $stream = [IO.File]::Open($path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
    try { $bytes = $utf8.GetBytes($Text); $stream.Write($bytes, 0, $bytes.Length); $stream.Flush($true) }
    finally { $stream.Dispose() }
}

function Get-TextDigest([string]$Text)
{
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($algorithm.ComputeHash($utf8.GetBytes($Text)))).Replace('-', '').ToLowerInvariant() }
    finally { $algorithm.Dispose() }
}

function Invoke-InventoryCommand([string]$Command, [string[]]$Arguments, [string]$ErrorFile)
{
    $ErrorActionPreference = 'Continue'
    $captured = @(& $Command @Arguments 2>> (Join-Path $outputPath $ErrorFile))
    $script:inventoryExit = $LASTEXITCODE
    return $captured
}

function Get-ArtifactIdentity([string[]]$Paths)
{
    $files = @()
    foreach ($path in $Paths)
    {
        if (-not (Test-Path -LiteralPath $path)) { throw "Identity artifact is unavailable: $path" }
        $item = Get-Item -LiteralPath $path
        if ($item.PSIsContainer) { $files += @(Get-ChildItem -LiteralPath $item.FullName -Recurse -File) }
        else { $files += $item }
    }
    $entries = @($files | Sort-Object FullName -Unique | ForEach-Object {
        $name = $_.FullName.Replace('\', '/')
        $prefix = $sourceRoot.Replace('\', '/') + '/'
        if ($name.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { $name = $name.Substring($prefix.Length) }
        [ordered]@{ path = $name; sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(); bytes = $_.Length }
    })
    return [ordered]@{ digest = Get-TextDigest (ConvertTo-Json -InputObject $entries -Depth 8 -Compress); artifacts = $entries }
}

$selectedCount = 0
$requirements = @()
$started = [DateTime]::UtcNow
try
{
    $registry = Get-Content -LiteralPath $RegistryPath -Raw | ConvertFrom-Json
    if ($registry.schemaVersion -ne 1) { throw 'Unsupported evaluation registry schema.' }
    $change = @($registry.changes | Where-Object { $_.id -eq $ChangeId })
    if ($change.Count -ne 1) { throw "Unknown or duplicate change ID: $ChangeId; zero tests selected." }
    $change = $change[0]
    $requirements = @($change.requiredEvidence | ForEach-Object {
        [ordered]@{ id = $_.id; kind = $_.kind; status = 'pending' }
    })
    $testIds = @($change.automatedTests | ForEach-Object { $_.id })
    if ($testIds.Count -eq 0 -or @($testIds | Sort-Object -Unique).Count -ne $testIds.Count) { throw 'Empty or duplicate registered test IDs.' }
    if (-not $BuildDirectory)
    {
        $presets = Get-Content -LiteralPath (Join-Path $sourceRoot 'CMakePresets.json') -Raw | ConvertFrom-Json
        $configure = @($presets.configurePresets | Where-Object { $_.name -eq $Preset })
        if ($configure.Count -ne 1) { throw "Unknown preset: $Preset" }
        $BuildDirectory = $configure[0].binaryDir.Replace('${sourceDir}', $sourceRoot)
    }
    $BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
    if (-not (Test-Path -LiteralPath $BuildDirectory -PathType Container)) { throw 'Configured build directory is unavailable.' }
    $inventoryRaw = (Invoke-InventoryCommand 'ctest' @('--test-dir', $BuildDirectory, '--show-only=json-v1') 'inventory-stderr.txt') -join "`n"
    if ($inventoryExit -ne 0) { throw 'CTest inventory discovery failed.' }
    Write-Once 'inventory.json' $inventoryRaw
    $inventory = $inventoryRaw | ConvertFrom-Json
    $selected = @()
    $missing = @()
    foreach ($id in $testIds)
    {
        $matches = @($inventory.tests | Where-Object {
            $labels = @($_.properties | Where-Object { $_.name -eq 'LABELS' } | ForEach-Object { $_.value })
            $labels -contains $id
        })
        if ($matches.Count -eq 0) { $missing += $id }
        $selected += $matches
    }
    $selected = @($selected | Sort-Object name -Unique)
    $selectedCount = $selected.Count

    $relevantPaths = @('Public', 'Private', 'Desktop', 'Tools', 'Tests', 'Config', 'docs', 'CMakeLists.txt', 'CMakePresets.json', '.clang-format', 'AGENTS.md')
    $commit = (Invoke-InventoryCommand 'git' @('-C', $sourceRoot, 'rev-parse', 'HEAD') 'git-stderr.txt') -join ''
    if ($inventoryExit -ne 0) { throw 'Source commit cannot be established.' }
    $patch = (Invoke-InventoryCommand 'git' (@('-C', $sourceRoot, 'diff', '--binary', '--no-ext-diff', 'HEAD', '--') + $relevantPaths) 'git-stderr.txt') -join "`n"
    if ($inventoryExit -ne 0) { throw 'Source patch cannot be established.' }
    $status = @(Invoke-InventoryCommand 'git' (@('-C', $sourceRoot, 'status', '--porcelain', '--untracked-files=all', '--') + $relevantPaths) 'git-stderr.txt')
    if ($inventoryExit -ne 0) { throw 'Source status cannot be established.' }
    $untrackedPaths = @(Invoke-InventoryCommand 'git' (@('-C', $sourceRoot, '-c', 'core.quotepath=false', 'ls-files', '--others', '--exclude-standard', '--') + $relevantPaths) 'git-stderr.txt')
    if ($inventoryExit -ne 0) { throw 'Untracked source inventory cannot be established.' }
    $untrackedFiles = @($untrackedPaths | ForEach-Object { Join-Path $sourceRoot $_ })
    $untracked = Get-ArtifactIdentity $untrackedFiles
    Write-Once 'source.patch' $patch
    $sourceIdentity = [ordered]@{ commit = $commit; patchDigest = Get-TextDigest $patch; untracked = $untracked.artifacts }
    $sourceDigest = Get-TextDigest (ConvertTo-Json -InputObject $sourceIdentity -Depth 8 -Compress)
    $buildFiles = @()
    $cachePath = Join-Path $BuildDirectory 'CMakeCache.txt'
    if (Test-Path -LiteralPath $cachePath) { $buildFiles += $cachePath }
    foreach ($test in $selected)
    {
        if ($test.command.Count -gt 0 -and (Test-Path -LiteralPath $test.command[0] -PathType Leaf)) { $buildFiles += $test.command[0] }
    }
    $buildIdentity = Get-ArtifactIdentity $buildFiles
    $buildDigest = Get-TextDigest (ConvertTo-Json -InputObject ([ordered]@{ preset = $Preset; inventoryDigest = Get-TextDigest $inventoryRaw; files = $buildIdentity.artifacts }) -Depth 8 -Compress)
    $providerIdentity = Get-ArtifactIdentity $ProviderFiles
    if ($ProviderFiles.Count -gt 0 -and $providerIdentity.artifacts.Count -eq 0) { throw 'Provider paths contain no actual artifacts.' }
    if ($SettingsFiles.Count -eq 0) { $SettingsFiles = @(Join-Path $sourceRoot 'Config') }
    if ($FixtureFiles.Count -eq 0) { $FixtureFiles = @(Join-Path $sourceRoot 'Tests/Fixtures') }
    $settingsIdentity = Get-ArtifactIdentity $SettingsFiles
    $fixtureIdentity = Get-ArtifactIdentity $FixtureFiles
    $hardware = [ordered]@{ machine = [Environment]::MachineName; os = [Environment]::OSVersion.VersionString; cpu = $env:PROCESSOR_IDENTIFIER; logicalProcessors = [Environment]::ProcessorCount }
    try
    {
        $hardware['memoryBytes'] = (Get-CimInstance Win32_ComputerSystem -ErrorAction Stop).TotalPhysicalMemory
        $hardware['graphics'] = @(Get-CimInstance Win32_VideoController -ErrorAction Stop | ForEach-Object {
            [ordered]@{ name = $_.Name; driver = $_.DriverVersion; reportedMemoryBytes = $_.AdapterRAM }
        })
    }
    catch { $hardware['extendedInventory'] = 'unavailable' }
    $manifest = [ordered]@{
        schemaVersion = 1; campaignId = $CampaignId; commit = $commit; sourceDigest = $sourceDigest; sourceDirty = ($status.Count -gt 0)
        buildDigest = $buildDigest; providerDigest = $(if ($ProviderFiles.Count -gt 0) { $providerIdentity.digest } else { '' })
        providerAvailable = ($ProviderFiles.Count -gt 0); settingsDigest = $settingsIdentity.digest; fixtureDigest = $fixtureIdentity.digest
        oracleVersion = $OracleVersion; hardware = (ConvertTo-Json -InputObject $hardware -Compress); seed = $Seed; timingBoundary = $TimingBoundary
        seedMechanism = 'declared seed forwarded as REVIA_EVALUATION_SEED'; seedVerified = $false
        capturedAt = $started.ToString('o'); preset = $Preset; changeId = $ChangeId; sourceIdentity = $sourceIdentity
        buildIdentity = $buildIdentity.artifacts; providerIdentity = $providerIdentity.artifacts; settingsIdentity = $settingsIdentity.artifacts; fixtureIdentity = $fixtureIdentity.artifacts
        registryDigest = (Get-FileHash -LiteralPath $RegistryPath -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    Write-Once 'manifest.json' (ConvertTo-Json -InputObject $manifest -Depth 12)
    if ($missing.Count -gt 0 -or $selectedCount -eq 0) { throw ('CTest labels absent or removed: ' + ($missing -join ', ') + "; selected count: $selectedCount") }
    $selector = '^(' + (($selected | ForEach-Object { [regex]::Escape($_.name) }) -join '|') + ')$'
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $env:REVIA_EVALUATION_SEED = $Seed.ToString([Globalization.CultureInfo]::InvariantCulture)
    $ErrorActionPreference = 'Continue'
    & ctest --test-dir $BuildDirectory --no-tests=error --output-on-failure -R $selector --output-junit (Join-Path $outputPath 'results.xml') *> (Join-Path $outputPath 'ctest.log')
    $testExit = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    $timer.Stop()
    $cases = @()
    if (Test-Path -LiteralPath (Join-Path $outputPath 'results.xml'))
    {
        [xml]$junit = Get-Content -LiteralPath (Join-Path $outputPath 'results.xml') -Raw
        $cases = @($junit.SelectNodes('//testcase'))
    }
    $perTest = @($cases | ForEach-Object {
        $state = 'passed'
        if ($_.SelectSingleNode('skipped') -or $_.GetAttribute('status') -in @('notrun', 'disabled', 'skipped')) { $state = 'unavailable' }
        elseif ($_.SelectSingleNode('failure') -or $_.SelectSingleNode('error')) { $state = 'failed' }
        [ordered]@{ name = $_.GetAttribute('name'); status = $state; seconds = $_.GetAttribute('time') }
    })
    foreach ($test in $selected)
    {
        if (@($perTest | Where-Object { $_.name -eq $test.name }).Count -eq 0)
        {
            $perTest += [ordered]@{ name = $test.name; status = 'unavailable'; seconds = '' }
        }
    }
    $unavailableCount = @($perTest | Where-Object { $_.status -eq 'unavailable' }).Count
    $status = if ($unavailableCount -eq $selectedCount) { 'incomplete' } elseif ($testExit -ne 0) { 'failed' } elseif ($unavailableCount -gt 0) { 'incomplete' } else { 'passed' }
    if ($testExit -eq 0 -and $unavailableCount -gt 0) { $testExit = 2 }
    $result = [ordered]@{
        schemaVersion = 1; campaignId = $CampaignId; changeId = $ChangeId; status = $status
        selectedCount = $selectedCount; selectedTests = @($selected | ForEach-Object { $_.name }); ctestExit = $testExit
        executedCount = $selectedCount - $unavailableCount; unavailableCount = $unavailableCount; perTestResults = $perTest
        elapsedMilliseconds = $timer.Elapsed.TotalMilliseconds; timingBoundary = $TimingBoundary; qualification = 'pending'
        requiredEvidence = $requirements; automatedQualification = @($change.automatedTests); manifestDigest = Get-TextDigest ([IO.File]::ReadAllText((Join-Path $outputPath 'manifest.json')))
    }
    Write-Once 'results.json' (ConvertTo-Json -InputObject $result -Depth 8)
    Write-Output "Campaign $CampaignId executed $selectedCount tests; qualification remains pending. Output: $outputPath"
    exit $testExit
}
catch
{
    $failure = [ordered]@{ schemaVersion = 1; campaignId = $CampaignId; changeId = $ChangeId; status = 'failed'; selectedCount = $selectedCount; qualification = 'pending'; requiredEvidence = $requirements; error = $_.Exception.Message }
    if (-not (Test-Path -LiteralPath (Join-Path $outputPath 'results.json'))) { Write-Once 'results.json' (ConvertTo-Json -InputObject $failure -Depth 8) }
    Write-Error $_.Exception.Message -ErrorAction Continue
    exit 1
}
