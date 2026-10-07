param([string]$Runner = (Join-Path $PSScriptRoot '../Tools/RunArchitectureEvaluation.ps1'))
$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $Runner)) { throw 'Campaign runner is missing; known selector cannot execute.' }
$taskDirectory = Join-Path ([IO.Path]::GetTempPath()) ('revia-campaign-check-' + [guid]::NewGuid())
New-Item -ItemType Directory -Path $taskDirectory | Out-Null
try {
    $project = Join-Path $taskDirectory 'project'
    $build = Join-Path $taskDirectory 'build'
    New-Item -ItemType Directory -Path $project | Out-Null
    @'
cmake_minimum_required(VERSION 3.20)
project(CampaignFixture NONE)
enable_testing()
add_test(NAME FixtureKnown COMMAND "${CMAKE_COMMAND}" -E true)
set_tests_properties(FixtureKnown PROPERTIES LABELS "T-FND-02-A;FND-02")
'@ | Set-Content -LiteralPath (Join-Path $project 'CMakeLists.txt') -Encoding UTF8
    & cmake -G Ninja -S $project -B $build *> (Join-Path $taskDirectory 'configure.log')
    if ($LASTEXITCODE -ne 0) { throw 'Actual CTest fixture configure failed.' }
    $registry = Join-Path $taskDirectory 'registry.json'
    '{"schemaVersion":1,"changes":[{"id":"FND-02","automatedTests":[{"id":"T-FND-02-A","qualification":"fixture"}],"requiredEvidence":[{"id":"HOST-BASELINE","kind":"live"}]}]}' | Set-Content -LiteralPath $registry -Encoding UTF8
    $shell = (Get-Process -Id $PID).Path
    $foreign = Join-Path $taskDirectory 'foreign-source'
    $ErrorActionPreference = 'Continue'
    & $shell -NoProfile -File $Runner -Preset debug -CampaignId foreign -ChangeId FND-02 -OutputDirectory $foreign -BuildDirectory $build -RegistryPath $registry *> (Join-Path $taskDirectory 'foreign.log')
    $foreignExit = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($foreignExit -eq 0) { throw 'An unrelated project was stamped as a Revia source build.' }
    $known = Join-Path $taskDirectory 'known'
    $ErrorActionPreference = 'Continue'
    & $shell -NoProfile -File $Runner -Preset debug -CampaignId known -ChangeId FND-02 -OutputDirectory $known -BuildDirectory $build -RegistryPath $registry -FixtureOnly *> (Join-Path $taskDirectory 'known.log')
    $knownExit = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($knownExit -ne 0) { throw ('Known selector failed: ' + (Get-Content -LiteralPath (Join-Path $taskDirectory 'known.log') -Raw)) }
    $manifest = Get-Content -LiteralPath (Join-Path $known 'manifest.json') -Raw | ConvertFrom-Json
    $results = Get-Content -LiteralPath (Join-Path $known 'results.json') -Raw | ConvertFrom-Json
    if ($results.selectedCount -ne 1 -or $results.qualification -ne 'pending' -or $results.requiredEvidence[0].status -ne 'pending') { throw 'Fixture execution claimed live qualification or omitted evidence.' }
    if ($manifest.providerAvailable -or $manifest.providerDigest -ne '') { throw 'Missing model was presented as an actual model identity.' }
    if ($manifest.seedVerified -ne $false) { throw 'Declared seed was reported as backend-verified.' }
    $identityFile = Join-Path (Split-Path -Parent $PSScriptRoot) ('Tests/Fixtures/Evaluation/campaign-source-' + [guid]::NewGuid() + '.txt')
    try {
        'Independent untracked source identity.' | Set-Content -LiteralPath $identityFile -Encoding UTF8
        $changed = Join-Path $taskDirectory 'changed-source'
        & $shell -NoProfile -File $Runner -Preset debug -CampaignId changed -ChangeId FND-02 -OutputDirectory $changed -BuildDirectory $build -RegistryPath $registry -FixtureOnly *> (Join-Path $taskDirectory 'changed.log')
        if ($LASTEXITCODE -ne 0) { throw 'Changed-source selector failed.' }
        $dirty = Get-Content -LiteralPath (Join-Path $changed 'manifest.json') -Raw | ConvertFrom-Json
        if (-not $dirty.sourceDirty -or $dirty.commit -ne $manifest.commit -or $dirty.sourceDigest -eq $manifest.sourceDigest) { throw 'Untracked source was mislabeled as an exact commit.' }
    } finally { Remove-Item -LiteralPath $identityFile -Force -ErrorAction SilentlyContinue }
    $original = [IO.File]::ReadAllBytes((Join-Path $known 'manifest.json'))
    $ErrorActionPreference = 'Continue'
    & $shell -NoProfile -File $Runner -Preset debug -CampaignId known -ChangeId FND-02 -OutputDirectory $known -BuildDirectory $build -RegistryPath $registry -FixtureOnly *> (Join-Path $taskDirectory 'overwrite.log')
    $runnerExit = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($runnerExit -eq 0 -or [Convert]::ToBase64String($original) -ne [Convert]::ToBase64String([IO.File]::ReadAllBytes((Join-Path $known 'manifest.json')))) { throw 'Immutable campaign was overwritten.' }
    $ErrorActionPreference = 'Continue'
    & $shell -NoProfile -File $Runner -Preset debug -CampaignId missing -ChangeId UNKNOWN -OutputDirectory (Join-Path $taskDirectory 'missing') -BuildDirectory $build -RegistryPath $registry -FixtureOnly *> (Join-Path $taskDirectory 'missing.log')
    $runnerExit = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($runnerExit -eq 0) { throw 'Missing selector passed.' }
    $removed = Join-Path $taskDirectory 'removed'
    '# Test registration deliberately removed.' | Set-Content -LiteralPath (Join-Path $build 'CTestTestfile.cmake') -Encoding UTF8
    $ErrorActionPreference = 'Continue'
    & $shell -NoProfile -File $Runner -Preset debug -CampaignId removed -ChangeId FND-02 -OutputDirectory $removed -BuildDirectory $build -RegistryPath $registry -FixtureOnly *> (Join-Path $taskDirectory 'removed.log')
    $runnerExit = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($runnerExit -eq 0) { throw 'Removed label passed.' }
    $empty = Get-Content -LiteralPath (Join-Path $removed 'results.json') -Raw | ConvertFrom-Json
    if ($empty.selectedCount -ne 0 -or $empty.status -ne 'failed') { throw 'Removed registration was not recorded as zero selected and failed.' }
    Add-Content -LiteralPath (Join-Path $project 'CMakeLists.txt') -Value 'set_tests_properties(FixtureKnown PROPERTIES DISABLED TRUE)'
    & cmake -G Ninja -S $project -B $build *> (Join-Path $taskDirectory 'disabled-configure.log')
    if ($LASTEXITCODE -ne 0) { throw 'Disabled-test fixture configure failed.' }
    $disabled = Join-Path $taskDirectory 'disabled'
    $ErrorActionPreference = 'Continue'
    & $shell -NoProfile -File $Runner -Preset debug -CampaignId disabled -ChangeId FND-02 -OutputDirectory $disabled -BuildDirectory $build -RegistryPath $registry -FixtureOnly *> (Join-Path $taskDirectory 'disabled.log')
    $runnerExit = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    $unavailable = Get-Content -LiteralPath (Join-Path $disabled 'results.json') -Raw | ConvertFrom-Json
    if ($runnerExit -eq 0 -or $unavailable.unavailableCount -ne 1 -or $unavailable.status -ne 'incomplete') { throw ('Disabled case silently became a completed automated pass: ' + ($unavailable | ConvertTo-Json -Depth 8) + (Get-Content -LiteralPath (Join-Path $disabled 'results.xml') -Raw)) }
    Write-Output 'Campaign runner checks passed: known, missing, removed, immutable and pending evidence.'
} finally {
    $resolved = [IO.Path]::GetFullPath($taskDirectory)
    if (-not $resolved.StartsWith([IO.Path]::GetFullPath([IO.Path]::GetTempPath()), [StringComparison]::OrdinalIgnoreCase)) { throw 'Fixture cleanup escaped temporary directory.' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
