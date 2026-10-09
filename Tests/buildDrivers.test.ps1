param([string]$EvidenceDirectory = 'build/repair-evidence/build/driver-tests')
$ErrorActionPreference = 'Stop'
$repoPath = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$driver = Join-Path $repoPath 'Tools/Build/InvokeCheckedCTest.ps1'
$buildDriver = Join-Path $repoPath 'Tools/Build/InvokeMeasuredBuild.ps1'
$shell = (Get-Process -Id $PID).Path
New-Item -ItemType Directory -Force -Path $EvidenceDirectory | Out-Null
$evidencePath = (Resolve-Path $EvidenceDirectory).Path
$fixtureRoot = Join-Path $evidencePath ('run-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $fixtureRoot | Out-Null
function RunDriver([string]$Script, [string[]]$Arguments, [string]$LogPath)
{
    $values = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $Script) + $Arguments
    $quoted = $values | ForEach-Object { '"' + $_.Replace('"', '\"') + '"' }
    $process = Start-Process -FilePath $shell -ArgumentList $quoted -WindowStyle Hidden -PassThru -Wait -RedirectStandardOutput ($LogPath + '.stdout') -RedirectStandardError ($LogPath + '.stderr')
    return $process.ExitCode
}
. (Join-Path $repoPath 'Tools/Build/FileHash.ps1')
$knownPath = Join-Path $fixtureRoot 'sha256-known-answer.txt'
[IO.File]::WriteAllBytes($knownPath, [Text.Encoding]::UTF8.GetBytes('abc'))
$knownHash = Get-ReviaFileHash -LiteralPath $knownPath
$pipedHash = Get-Item -LiteralPath $knownPath | Get-ReviaFileHash
if ($knownHash.Hash -ne 'BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD' -or $pipedHash.Hash -ne $knownHash.Hash -or $pipedHash.Path -ne $knownHash.Path) { throw 'SHA256 known-answer or pipeline hash failed.' }
$knownHash | ConvertTo-Json | Set-Content (Join-Path $fixtureRoot 'file-hash-known-answer.json')
$results = @()
foreach ($case in @('empty', 'missing', 'extra', 'failure', 'skipped', 'untrusted-source', 'hash-command-unavailable', 'success'))
{
    $fixturePath = Join-Path $fixtureRoot $case
    $sourcePath = $fixturePath
    if ($case -eq 'untrusted-source') { $sourcePath = Join-Path ([IO.Path]::GetTempPath()) ('revia-missing-source-' + [guid]::NewGuid().ToString('N')) }
    New-Item -ItemType Directory -Force -Path @($fixturePath, $sourcePath) | Out-Null
    $source = "cmake_minimum_required(VERSION 3.20)`nproject(CTestDriverFixture NONE)`nenable_testing()`n"
    $expected = @('Fixture.Pass')
    if ($case -ne 'empty') { $source += 'add_test(NAME Fixture.Pass COMMAND "${CMAKE_COMMAND}" -E true)' + "`n" }
    if ($case -eq 'missing') { $expected += 'Fixture.Missing' }
    if ($case -eq 'extra') { $source += 'add_test(NAME Fixture.Extra COMMAND "${CMAKE_COMMAND}" -E true)' + "`n" }
    if ($case -eq 'failure') { $source = $source.Replace('-E true', '-E false') }
    if ($case -eq 'skipped') { $source += "set_tests_properties(Fixture.Pass PROPERTIES DISABLED TRUE)`n" }
    if ($case -eq 'success') { $source += 'add_test(NAME Fixture.Second COMMAND "${CMAKE_COMMAND}" -E true)' + "`n"; $expected += 'Fixture.Second' }
    $source | Set-Content (Join-Path $sourcePath 'CMakeLists.txt')
    if ($sourcePath -ne $fixturePath) { $source | Set-Content (Join-Path $fixturePath 'source.CMakeLists.txt') }
    $expected | Set-Content (Join-Path $fixturePath 'expected.txt')
    & cmake -S $sourcePath -B (Join-Path $fixturePath 'build') -G Ninja *> (Join-Path $fixturePath 'configure.log')
    if ($LASTEXITCODE -ne 0) { throw "Fixture configure failed: $case" }
    $checkedDriver = $driver
    if ($case -eq 'hash-command-unavailable')
    {
        $checkedDriver = Join-Path $fixturePath 'hash-command-unavailable.ps1'
        $wrapper = "param([string]`$BuildDirectory, [string]`$ExpectedInventory, [string]`$EvidenceDirectory)`nfunction global:Get-FileHash { throw 'Get-FileHash unavailable fixture' }`n& '" + $driver.Replace("'", "''") + "' -BuildDirectory `$BuildDirectory -ExpectedInventory `$ExpectedInventory -EvidenceDirectory `$EvidenceDirectory`nexit `$LASTEXITCODE"
        $wrapper | Set-Content $checkedDriver
    }
    $actualExit = RunDriver $checkedDriver @('-BuildDirectory', (Join-Path $fixturePath 'build'), '-ExpectedInventory', (Join-Path $fixturePath 'expected.txt'), '-EvidenceDirectory', (Join-Path $fixturePath 'results')) (Join-Path $fixturePath 'driver.log')
    $expectedExit = if ($case -in @('success', 'hash-command-unavailable')) { 0 } else { 1 }
    if ($actualExit -ne $expectedExit) { throw "$case expected exit $expectedExit, got $actualExit" }
    $summary = Get-Content (Join-Path $fixturePath 'results/test-result.json') -Raw | ConvertFrom-Json
    if ($case -eq 'untrusted-source' -and ($summary.identityTrusted -or $summary.validationExitCode -eq 0)) { throw 'Unidentified source passed qualification.' }
    if ($case -eq 'success' -and (-not $summary.identityTrusted -or $summary.executedCount -ne 2 -or $summary.skippedCount -ne 0 -or $summary.failedCount -ne 0)) { throw 'Success fixture lost executed counts.' }
    if ($case -eq 'failure' -and ($summary.failedCount -ne 1 -or $summary.ctestExitCode -eq 0)) { throw 'Failed CTest was not retained.' }
    if ($case -eq 'skipped' -and $summary.skippedCount -ne 1) { throw 'Skipped CTest was not retained.' }
    $results += [ordered]@{ case = $case; exitCode = $actualExit; expectedExitCode = $expectedExit; result = $summary }
}
$interruptPath = Join-Path $fixtureRoot 'interrupted-build'
New-Item -ItemType Directory -Force -Path $interruptPath | Out-Null
$source = "cmake_minimum_required(VERSION 3.20)`nproject(InterruptedBuildFixture NONE)`n" + 'add_custom_target(wait ALL COMMAND "${CMAKE_COMMAND}" -E sleep 20)'
$source | Set-Content (Join-Path $interruptPath 'CMakeLists.txt')
& cmake -S $interruptPath -B (Join-Path $interruptPath 'build') -G Ninja *> (Join-Path $interruptPath 'configure.log')
if ($LASTEXITCODE -ne 0) { throw 'Interrupted build fixture configure failed.' }
$originalPath = $env:PATH
$cmakeDirectory = Split-Path ((Get-Command cmake -CommandType Application | Select-Object -First 1).Source)
$env:PATH = $cmakeDirectory.Replace('\', '/') + ';' + $cmakeDirectory.Replace('/', '\') + ';' + $originalPath
try
{
    $actualExit = RunDriver $buildDriver @('-BuildDirectory', (Join-Path $interruptPath 'build'), '-Parallel', '1', '-TimeoutSeconds', '1', '-EvidenceDirectory', (Join-Path $interruptPath 'results')) (Join-Path $interruptPath 'driver.log')
}
finally
{
    $env:PATH = $originalPath
}
if ($actualExit -ne 124) { throw "Interrupted build expected exit124, got $actualExit" }
$interrupted = Get-Content (Join-Path $interruptPath 'results/build-result.json') -Raw | ConvertFrom-Json
if (-not $interrupted.timedOut -or -not (Test-Path (Join-Path $interruptPath 'results/build.stdout.log')) -or -not (Test-Path (Join-Path $interruptPath 'results/build.stderr.log'))) { throw 'Interrupted build did not preserve logs and state.' }
$results += [ordered]@{ case = 'interrupted-build'; result = $interrupted }
$results | ConvertTo-Json -Depth 7 | Set-Content (Join-Path $evidencePath 'results.json')
Write-Output 'Passed 9 build-driver fixtures: empty, missing, extra, failed, skipped, untrusted source, unavailable hash command, complete inventory and duplicate-PATH interrupted build.'
