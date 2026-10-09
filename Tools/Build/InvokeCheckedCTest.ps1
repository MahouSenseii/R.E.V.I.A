param(
    [Parameter(Mandatory = $true)][string]$BuildDirectory,
    [Parameter(Mandatory = $true)][string]$EvidenceDirectory,
    [string]$ExpectedInventory = (Join-Path $PSScriptRoot 'ctest-windows-expected.txt'),
    [int]$TimeoutSeconds = 600
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $EvidenceDirectory | Out-Null
$evidencePath = (Resolve-Path -LiteralPath $EvidenceDirectory).Path
$buildPath = (Resolve-Path -LiteralPath $BuildDirectory).Path
$started = [DateTime]::UtcNow
$watch = [Diagnostics.Stopwatch]::StartNew()
$ctestExit = $null
$discoveryExit = $null
$validationExit = 1
$discovered = @()
$expected = @()
$executed = @()
$skipped = @()
$failed = @()
$diagnostic = ''
try
{
    $expected = @(Get-Content -LiteralPath $ExpectedInventory | Where-Object { $_.Trim() -and -not $_.StartsWith('#') })
    if ($expected.Count -eq 0 -or @($expected | Select-Object -Unique).Count -ne $expected.Count) { throw 'Expected test inventory is empty or contains duplicates.' }
    $expected | Set-Content (Join-Path $evidencePath 'expected-tests.txt')
    & ctest --test-dir $buildPath --show-only=json-v1 2> (Join-Path $evidencePath 'discovery.stderr.log') | Set-Content (Join-Path $evidencePath 'discovered-tests.json')
    $discoveryExit = $LASTEXITCODE
    if ($discoveryExit -ne 0) { throw "CTest discovery failed with exit $discoveryExit." }
    $inventory = Get-Content (Join-Path $evidencePath 'discovered-tests.json') -Raw | ConvertFrom-Json
    $discovered = @($inventory.tests | ForEach-Object { $_.name })
    if ($discovered.Count -eq 0) { throw 'CTest discovered zero tests.' }
    $difference = @(Compare-Object -ReferenceObject ($expected | Sort-Object) -DifferenceObject ($discovered | Sort-Object) -CaseSensitive)
    if ($difference.Count -gt 0) { $difference | ConvertTo-Json | Set-Content (Join-Path $evidencePath 'inventory-difference.json'); throw 'Discovered tests do not match the complete expected inventory.' }
    $junitPath = Join-Path $evidencePath 'ctest.junit.xml'
    $ErrorActionPreference = 'Continue'
    & ctest --test-dir $buildPath --output-on-failure --no-tests=error --timeout $TimeoutSeconds --output-junit $junitPath 2>&1 | Tee-Object (Join-Path $evidencePath 'ctest.log')
    $ctestExit = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if (-not (Test-Path -LiteralPath $junitPath)) { throw 'CTest did not produce JUnit results.' }
    [xml]$junit = Get-Content -LiteralPath $junitPath -Raw
    $cases = @($junit.SelectNodes('//testcase'))
    $executed = @($cases | Where-Object { -not $_.SelectSingleNode('skipped') -and $_.status -notin @('disabled', 'notrun') } | ForEach-Object { $_.name })
    $skipped = @($cases | Where-Object { $_.SelectSingleNode('skipped') -or $_.status -in @('disabled', 'notrun') } | ForEach-Object { $_.name })
    $failed = @($cases | Where-Object { $_.SelectSingleNode('failure') -or $_.SelectSingleNode('error') } | ForEach-Object { $_.name })
    $resultDifference = @(Compare-Object -ReferenceObject ($expected | Sort-Object) -DifferenceObject (@($cases | ForEach-Object { $_.name }) | Sort-Object) -CaseSensitive)
    if ($resultDifference.Count -gt 0 -or $executed.Count -eq 0 -or $skipped.Count -gt 0) { throw 'Executed JUnit inventory is incomplete or contains skipped tests.' }
    if ($ctestExit -ne 0 -or $failed.Count -gt 0) { throw "CTest failed: exit=$ctestExit failed=$($failed.Count)." }
    $validationExit = 0
}
catch
{
    $diagnostic = $_.Exception.Message
    Write-Error $diagnostic -ErrorAction Continue
}
finally
{
    $watch.Stop()
    $identityTrusted = $false
    $identityExits = [ordered]@{}
    try
    {
        $cache = Get-Content (Join-Path $buildPath 'CMakeCache.txt')
        $homeEntry = $cache | Where-Object { $_ -match '^CMAKE_HOME_DIRECTORY:INTERNAL=' } | Select-Object -First 1
        if (-not $homeEntry) { throw 'Build configuration does not identify its source directory.' }
        $buildSource = $homeEntry.Substring($homeEntry.IndexOf('=') + 1)
        $sourcePath = (& git -C $buildSource rev-parse --show-toplevel 2> (Join-Path $evidencePath 'source-root.stderr.log'))
        $identityExits['root'] = $LASTEXITCODE
        if ($LASTEXITCODE -ne 0 -or -not $sourcePath) { throw 'Cannot identify the configured source repository.' }
        $head = (& git -C $sourcePath rev-parse HEAD 2> (Join-Path $evidencePath 'source-head.stderr.log'))
        $identityExits['head'] = $LASTEXITCODE
        if ($LASTEXITCODE -ne 0 -or $head -notmatch '^[a-fA-F0-9]{40}$') { throw 'Cannot record a valid source HEAD.' }
        $head | Set-Content -Encoding utf8 (Join-Path $evidencePath 'source-head.txt')
        & git -C $sourcePath status --porcelain=v1 2> (Join-Path $evidencePath 'source-status.stderr.log') | Set-Content -Encoding utf8 (Join-Path $evidencePath 'source-status.txt')
        $identityExits['status'] = $LASTEXITCODE
        if ($LASTEXITCODE -ne 0) { throw 'Cannot record source dirty state.' }
        & git -c core.safecrlf=false -C $sourcePath diff --binary "--output=$(Join-Path $evidencePath 'source.diff')" 2> (Join-Path $evidencePath 'source-diff.stderr.log')
        $identityExits['diff'] = $LASTEXITCODE
        if ($LASTEXITCODE -ne 0) { throw 'Cannot retain the source diff.' }
        $sourceFiles = & git -C $sourcePath ls-files --cached --others --exclude-standard 2> (Join-Path $evidencePath 'source-files.stderr.log')
        $identityExits['files'] = $LASTEXITCODE
        if ($LASTEXITCODE -ne 0) { throw 'Cannot inventory source files.' }
        $manifest = @($sourceFiles | Where-Object { $_ -match '^(Public|Private|Desktop|Tests|Config|Tools|Assets)/|^CMakeLists.txt$|^\.gitignore$|^\.github/workflows/build-and-test.yml$' } | Sort-Object | ForEach-Object {
            $filePath = Join-Path $sourcePath $_
            if (Test-Path -LiteralPath $filePath -PathType Leaf) { [ordered]@{ path = $_; sha256 = (Get-FileHash -LiteralPath $filePath -Algorithm SHA256).Hash } }
        })
        $manifest | ConvertTo-Json -Depth 3 | Set-Content (Join-Path $evidencePath 'source-inputs.json')
        foreach ($name in @('CMakeCache.txt', 'compile_commands.json', '.ninja_log'))
        {
            $path = Join-Path $buildPath $name
            if (Test-Path -LiteralPath $path) { Copy-Item -LiteralPath $path -Destination $evidencePath }
        }
        @(Get-ChildItem -LiteralPath $buildPath -File | Where-Object { $_.Extension -in @('.exe', '.dll', '.a') } | Get-FileHash -Algorithm SHA256 | Select-Object Path,Hash) | ConvertTo-Json | Set-Content (Join-Path $evidencePath 'binary-hashes.json')
        $identityTrusted = $true
    }
    catch
    {
        $validationExit = 1
        $diagnostic = ($diagnostic + ' Source/build identity: ' + $_.Exception.Message).Trim()
        Write-Error $diagnostic -ErrorAction Continue
    }
    [ordered]@{ buildDirectory = $buildPath; startedUtc = $started.ToString('o'); finishedUtc = [DateTime]::UtcNow.ToString('o'); elapsedSeconds = $watch.Elapsed.TotalSeconds; expectedCount = $expected.Count; discoveredCount = $discovered.Count; executedCount = $executed.Count; skippedCount = $skipped.Count; failedCount = $failed.Count; discoveryExitCode = $discoveryExit; ctestExitCode = $ctestExit; identityTrusted = $identityTrusted; identityExitCodes = $identityExits; validationExitCode = $validationExit; diagnostic = $diagnostic; executed = $executed; skipped = $skipped; failed = $failed } | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $evidencePath 'test-result.json')
}
exit $validationExit