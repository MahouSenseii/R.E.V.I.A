param([string]$Runner = (Join-Path $PSScriptRoot '../Tools/RunCognitionLiveCampaign.ps1'))
$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $Runner)) { throw 'Cognition receipt runner is missing.' }
$taskDirectory = Join-Path ([IO.Path]::GetTempPath()) ('revia-cognition-receipts-' + [guid]::NewGuid())
New-Item -ItemType Directory -Path $taskDirectory | Out-Null
$server = $null
try {
    $source = Join-Path $taskDirectory 'source'
    $build = Join-Path $taskDirectory 'build'
    New-Item -ItemType Directory -Path (Join-Path $source 'Public'), (Join-Path $source 'Tests'), $build | Out-Null
    'original source' | Set-Content (Join-Path $source 'Public/source.txt') -Encoding UTF8
    & git -C $source init -q
    & git -C $source add Public/source.txt
    & git -C $source -c user.name=Fixture -c user.email=fixture@example.invalid commit -qm baseline
    if ($LASTEXITCODE -ne 0) { throw 'Disposable source fixture could not be established.' }
    'changed source' | Set-Content (Join-Path $source 'Public/source.txt') -Encoding UTF8
    'untracked source' | Set-Content (Join-Path $source 'Tests/untracked.txt') -Encoding UTF8
    "CMAKE_HOME_DIRECTORY:INTERNAL=$source" | Set-Content (Join-Path $build 'CMakeCache.txt') -Encoding UTF8
    $model = Join-Path $taskDirectory 'fixture.gguf'
    'No actual model: receipt hashing fixture.' | Set-Content $model -Encoding UTF8
    $corpus = Join-Path $taskDirectory 'corpus.json'
    '{"schemaVersion":1,"seeds":[11,29],"cases":[{"id":"fixture-one"},{"id":"fixture-two"}]}' | Set-Content $corpus -Encoding UTF8
    $profile = Join-Path $taskDirectory 'profile.json'
    '{"id":"fixture","systemPrompt":"Preserve authored fixture","fixtureMode":"success"}' | Set-Content $profile -Encoding UTF8
    $client = Join-Path $build 'fixture-client.ps1'
    @'
param($Profile, $Corpus, $Port, $Report, $Mode, $Manifest)
$authored = Get-Content -LiteralPath $Profile -Raw | ConvertFrom-Json
if ($Mode -ne 'cognition') { throw 'Actual cognition CLI mode was not forwarded.' }
$binding = Get-Content -LiteralPath $Manifest -Raw | ConvertFrom-Json
if ($binding.schemaVersion -ne 1 -or $binding.seed -ne 0 -or -not $binding.providerAvailable -or $binding.sourceDigest.Length -ne 64) { throw 'Typed manifest was not provided before CLI invocation.' }
if ($authored.fixtureMode -eq 'change') { Add-Content -LiteralPath $Profile -Value ' ' }
$result = @{ liveQualified = $false; expectedRuns = 4; recordedRuns = 4; missingRuns = 0; unavailable = 0 }
if ($authored.fixtureMode -eq 'fail') { $result.unavailable = 1 }
$result | ConvertTo-Json | Set-Content -LiteralPath $Report -Encoding UTF8
"$Mode|$Port" | Set-Content -LiteralPath ($Report + '.invocation.txt') -Encoding UTF8
if ($authored.fixtureMode -eq 'fail') { exit 2 }
exit 0
'@ | Set-Content $client -Encoding UTF8
    $serverScript = Join-Path $taskDirectory 'server.ps1'
    @'
param([int]$Port, [string]$Model)
$listener = New-Object Net.Sockets.TcpListener([Net.IPAddress]::Loopback, $Port)
$listener.Start()
try {
    while ($true) {
        $client = $listener.AcceptTcpClient()
        $stream = $client.GetStream()
        $buffer = New-Object byte[] 8192
        $request = ''
        while (-not $request.Contains("`r`n`r`n")) {
            $count = $stream.Read($buffer, 0, $buffer.Length)
            if ($count -le 0) { break }
            $request += [Text.Encoding]::ASCII.GetString($buffer, 0, $count)
        }
        if ($request.StartsWith('GET /v1/models ')) { $value = @{ data = @(@{ id = 'fixture-model' }) } }
        else { $value = @{ model_path = $Model; model_alias = 'fixture-model'; default_generation_settings = @{ n_ctx = 8192 } } }
        $bytes = [Text.Encoding]::UTF8.GetBytes(($value | ConvertTo-Json -Depth 8))
        $headers = [Text.Encoding]::ASCII.GetBytes("HTTP/1.1 200 OK`r`nContent-Type: application/json`r`nContent-Length: $($bytes.Length)`r`nConnection: close`r`n`r`n")
        $stream.Write($headers, 0, $headers.Length)
        $stream.Write($bytes, 0, $bytes.Length)
        $client.Close()
    }
} finally { $listener.Stop() }
'@ | Set-Content $serverScript -Encoding UTF8
    $socket = New-Object Net.Sockets.TcpListener([Net.IPAddress]::Loopback, 0)
    $socket.Start()
    $port = $socket.LocalEndpoint.Port
    $socket.Stop()
    $shell = (Get-Process -Id $PID).Path
    $server = Start-Process -FilePath $shell -ArgumentList @('-NoProfile', '-File', ('"' + $serverScript + '"'), $port, ('"' + $model + '"')) -WindowStyle Hidden -PassThru
    $ready = $false
    for ($attempt = 0; $attempt -lt 40; ++$attempt) {
        try { $null = Invoke-RestMethod "http://127.0.0.1:$port/v1/models" -TimeoutSec 1; $ready = $true; break }
        catch { Start-Sleep -Milliseconds 100 }
    }
    if (-not $ready) { throw 'Disposable metadata fixture server did not start.' }
    $launch = Join-Path $taskDirectory 'launch.json'
    @{ server = $shell; model = $model; pid = $server.Id; started = $server.StartTime.ToUniversalTime().ToString('o') } | ConvertTo-Json | Set-Content $launch -Encoding UTF8
    $settings = Join-Path $taskDirectory 'settings.json'
    '{"fixture":true}' | Set-Content $settings -Encoding UTF8
    $common = @('-BuildDirectory', $build, '-ExecutablePath', $client, '-SourceDirectory', $source,
        '-ProfilePath', $profile, '-CorpusPath', $corpus, '-Port', $port, '-LaunchMetadataPath', $launch,
        '-SettingsFiles', $settings, '-FixtureOnly')
    $known = Join-Path $taskDirectory 'known'
    $ErrorActionPreference = 'Continue'
    & $shell -NoProfile -File $Runner @common -CampaignId known -OutputDirectory $known *> (Join-Path $taskDirectory 'known.log')
    $knownExit = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($knownExit -ne 0) {
        $diagnostic = Get-Content (Join-Path $taskDirectory 'known.log') -Raw
        if (Test-Path (Join-Path $known 'results.json')) { $diagnostic += Get-Content (Join-Path $known 'results.json') -Raw }
        throw ('Receipt fixture failed: ' + $diagnostic)
    }
    $before = Get-Content (Join-Path $known 'before.json') -Raw | ConvertFrom-Json
    $after = Get-Content (Join-Path $known 'after.json') -Raw | ConvertFrom-Json
    $results = Get-Content (Join-Path $known 'results.json') -Raw | ConvertFrom-Json
    if (-not $before.source.sourceDirty -or $before.source.untracked.Count -ne 1 -or $before.source.commit.Length -ne 40) { throw 'Dirty/untracked source was mislabeled as an exact clean commit.' }
    if ($before.source.untracked[0].sha256.Length -ne 64 -or -not (Get-Content (Join-Path $known 'before.source.patch') -Raw).Contains('changed source')) { throw 'Exact dirty source evidence is missing.' }
    if ($before.profile.sha256.Length -ne 64 -or $before.corpus.sha256.Length -ne 64 -or $before.build.executable.sha256.Length -ne 64 -or $before.provider.model.sha256.Length -ne 64) { throw 'An actual artifact digest is missing.' }
    if ($before.expectedRuns -ne 4 -or ($before.seeds -join ',') -ne '11,29' -or $before.provider.observedModelId -ne 'fixture-model') { throw 'Corpus or observed provider identity was replaced with a guessed value.' }
    if (-not $results.provenanceStable -or $results.sourceBuildVerified -or $before.sourceBuildVerified -or $results.liveQualified -or $results.providerIdentityVerified -or $results.backendSeedVerified -or $results.qualification -ne 'fixture-only-unqualified' -or $results.independentSemanticReview -ne 'pending') { throw 'Receipt facts manufactured qualification or discarded pending review.' }
    if ($before.source.digest -ne $after.source.digest) { throw 'Unchanged fixture source changed identity.' }
    $receiptBytes = [IO.File]::ReadAllBytes((Join-Path $known 'before.json'))
    $aggregateBytes = [IO.File]::ReadAllBytes((Join-Path $known 'cognition.json'))
    $ErrorActionPreference = 'Continue'
    & $shell -NoProfile -File $Runner @common -CampaignId reused -OutputDirectory $known *> (Join-Path $taskDirectory 'reuse.log')
    $reuseExit = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($reuseExit -eq 0 -or [Convert]::ToBase64String($receiptBytes) -ne [Convert]::ToBase64String([IO.File]::ReadAllBytes((Join-Path $known 'before.json'))) -or [Convert]::ToBase64String($aggregateBytes) -ne [Convert]::ToBase64String([IO.File]::ReadAllBytes((Join-Path $known 'cognition.json')))) { throw 'Reused output replaced retained evidence.' }
    foreach ($mode in 'fail', 'change') {
        "{`"id`":`"fixture`",`"fixtureMode`":`"$mode`"}" | Set-Content $profile -Encoding UTF8
        $output = Join-Path $taskDirectory $mode
        $ErrorActionPreference = 'Continue'
        & $shell -NoProfile -File $Runner @common -CampaignId $mode -OutputDirectory $output *> (Join-Path $taskDirectory "$mode.log")
        $exit = $LASTEXITCODE
        $ErrorActionPreference = 'Stop'
        if ($exit -eq 0 -or -not (Test-Path (Join-Path $output 'after.json'))) { throw 'Failed or changed run lost its after receipt or returned success.' }
        $result = Get-Content (Join-Path $output 'results.json') -Raw | ConvertFrom-Json
        if ($mode -eq 'fail' -and $result.runnerExitCode -ne 2) { throw 'Actual CLI failure was not preserved.' }
        if ($mode -eq 'change' -and ($result.provenanceStable -or $result.mismatches -notcontains 'profile')) { throw 'Changed authored profile was not diagnosed by name.' }
    }
    $nativeClient = Join-Path $taskDirectory 'fresh-fixture-client.exe'
    $compile = Join-Path $taskDirectory 'compile-fixture.ps1'
    @'
param([string]$Output)
Add-Type -OutputAssembly $Output -OutputType ConsoleApplication -TypeDefinition @"
using System;
using System.IO;
public class ReceiptFixture
{
    public static int Main(string[] args)
    {
        if (args.Length != 6 || args[4] != "cognition" || !File.Exists(args[5])) return 2;
        File.WriteAllText(args[3], "{\"liveQualified\":false,\"fixtureBuild\":\"fresh\"}");
        return 0;
    }
}
"@
'@ | Set-Content $compile -Encoding UTF8
    & powershell.exe -NoProfile -File $compile -Output $nativeClient *> (Join-Path $taskDirectory 'compile.log')
    if ($LASTEXITCODE -ne 0) { throw 'Disposable compiled CLI fixture failed to build.' }
    $tools = Join-Path $source 'Tools'
    New-Item -ItemType Directory -Path $tools | Out-Null
    $copiedRunner = Join-Path $tools 'RunCognitionLiveCampaign.ps1'
    Copy-Item -LiteralPath $Runner -Destination $copiedRunner
    $nativeClientCmake = $nativeClient.Replace('\', '/')
    @"
cmake_minimum_required(VERSION 3.20)
project(OwnedReceiptFixture NONE)
add_custom_target(ReviaAnswerQualityLive
    COMMAND "`${CMAKE_COMMAND}" -E copy "$nativeClientCmake" "`${CMAKE_BINARY_DIR}/ReviaAnswerQualityLive.exe")
"@ | Set-Content (Join-Path $source 'CMakeLists.txt') -Encoding UTF8
    $nativeBuild = Join-Path $taskDirectory 'native-build'
    & cmake -G Ninja -S $source -B $nativeBuild *> (Join-Path $taskDirectory 'native-configure.log')
    if ($LASTEXITCODE -ne 0) { throw 'Disposable configured build fixture failed.' }
    'Stale executable: must be replaced by the target build.' | Set-Content (Join-Path $nativeBuild 'ReviaAnswerQualityLive.exe') -Encoding UTF8
    $built = Join-Path $taskDirectory 'rebuilt'
    $ErrorActionPreference = 'Continue'
    & $shell -NoProfile -File $copiedRunner -BuildDirectory $nativeBuild -CampaignId rebuilt -OutputDirectory $built -ProfilePath $profile -CorpusPath $corpus -Port $port -LaunchMetadataPath $launch -SettingsFiles $settings *> (Join-Path $taskDirectory 'rebuilt.log')
    $builtExit = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($builtExit -ne 0) {
        $diagnostic = Get-Content (Join-Path $taskDirectory 'rebuilt.log') -Raw
        if (Test-Path (Join-Path $built 'results.json')) { $diagnostic += Get-Content (Join-Path $built 'results.json') -Raw }
        throw ('Default build fixture failed: ' + $diagnostic)
    }
    $result = Get-Content (Join-Path $built 'results.json') -Raw | ConvertFrom-Json
    if (-not $result.sourceBuildVerified -or -not $result.provenanceStable -or $result.aggregate.fixtureBuild -ne 'fresh' -or $result.liveQualified) { throw 'Default runner admitted a stale executable or manufactured qualification.' }
    'Cognition receipt fixture passed: immutable provenance, failure/change retention and stale executable rebuild; no inference or qualification.'
} finally {
    if ($server -and -not $server.HasExited) { Stop-Process -Id $server.Id -Force }
    $resolved = [IO.Path]::GetFullPath($taskDirectory)
    if ($resolved.StartsWith([IO.Path]::GetFullPath([IO.Path]::GetTempPath()), [StringComparison]::OrdinalIgnoreCase)) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
