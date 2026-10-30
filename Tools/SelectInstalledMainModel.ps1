param(
    [ValidateSet('DryRun', 'Apply')][string]$Mode = 'DryRun',
    [Parameter(Mandatory = $true)][string]$SettingsPath,
    [Parameter(Mandatory = $true)][string]$ModelPath,
    [Parameter(Mandatory = $true)][string]$ProjectorPath,
    [Parameter(Mandatory = $true)][string]$ReceiptPath,
    [string]$ManifestPath = (Join-Path $PSScriptRoot '..\Config\model_manifest.json'),
    [string]$OwnerOverlayPath,
    [string]$ExpectedDryRunReceipt
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ReviaAcceleration.ps1')
$utf8 = [Text.UTF8Encoding]::new($false, $true)
$mainKeys = @('modelName', 'modelPath', 'multimodalProjectorPath')

function Require([bool]$value, [string]$reason) {
    if (-not $value) { throw $reason }
}

function Digest([byte[]]$bytes) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}

function ReadBounded([string]$path) {
    Require ((Get-Item -LiteralPath $path).Length -le 1048576) 'A selected JSON input exceeds 1 MiB.'
    $bytes = [IO.File]::ReadAllBytes($path)
    Require ($bytes.Length -le 1048576) 'A selected JSON input grew beyond 1 MiB.'
    return ,$bytes
}

function Text([byte[]]$bytes) {
    $offset = 0
    if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xef -and $bytes[1] -eq 0xbb -and $bytes[2] -eq 0xbf) { $offset = 3 }
    return $utf8.GetString($bytes, $offset, $bytes.Length - $offset)
}

function WriteNew([string]$path, [byte[]]$bytes) {
    $file = [IO.File]::Open($path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try { $file.Write($bytes, 0, $bytes.Length); $file.Flush($true) }
    finally { $file.Dispose() }
}

function WriteReceipt([string]$path, $value) {
    WriteNew $path ([byte[]]@())
    Save-ReviaJsonFile -Path $path -Value $value -Depth 16
}

function FileIdentity([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return @{path=$path;exists=$false;sha256='missing'} }
    Require (Test-Path -LiteralPath $path -PathType Leaf) 'A selected file path is not a regular file.'
    return @{path=$path;exists=$true;sha256=(Digest (ReadBounded $path))}
}

function ReadJsonValue($tokens, [ref]$index, [string]$path, $spans, [int]$depth = 0) {
    Require ($depth -le 64 -and $index.Value -lt $tokens.Count) 'JSON nesting is unsupported or incomplete.'
    $token = $tokens[$index.Value]
    $index.Value++
    if ($token.raw -eq '{') {
        $names = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
        if ($tokens[$index.Value].raw -ne '}') {
            while ($true) {
                $key = $tokens[$index.Value]
                Require ($key.raw.StartsWith('"')) 'Expected a JSON property name.'
                $name = ConvertFrom-Json -InputObject $key.raw
                Require ($names.Add($name)) 'Duplicate JSON property names are ambiguous; no settings were changed.'
                $index.Value++
                Require ($tokens[$index.Value].raw -eq ':') 'Expected a JSON property separator.'
                $index.Value++
                $child = $path + '/' + $name.Replace('~', '~0').Replace('/', '~1')
                if ($path -eq '/llm' -and $mainKeys -ccontains $name) {
                    $value = $tokens[$index.Value]
                    Require ($value.raw.StartsWith('"')) 'The three existing Main model fields must be strings.'
                    $spans[$name] = $value
                }
                ReadJsonValue $tokens $index $child $spans ($depth + 1)
                if ($tokens[$index.Value].raw -eq '}') { break }
                Require ($tokens[$index.Value].raw -eq ',') 'Expected a JSON property delimiter.'
                $index.Value++
            }
        }
        $index.Value++
    }
    elseif ($token.raw -eq '[') {
        if ($tokens[$index.Value].raw -ne ']') {
            while ($true) {
                ReadJsonValue $tokens $index ($path + '/[]') $spans ($depth + 1)
                if ($tokens[$index.Value].raw -eq ']') { break }
                Require ($tokens[$index.Value].raw -eq ',') 'Expected a JSON array delimiter.'
                $index.Value++
            }
        }
        $index.Value++
    }
}

function MainStringSpans([string]$text) {
    $regex = [regex]'\G\s*(?<token>"(?:[^"\\\u0000-\u001f]|\\(?:["\\/bfnrt]|u[0-9a-fA-F]{4}))*"|[{}\[\]:,]|true|false|null|-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?)'
    $tokens = [Collections.Generic.List[object]]::new()
    $offset = 0
    while ($offset -lt $text.Length) {
        if ([string]::IsNullOrWhiteSpace($text.Substring($offset))) { break }
        $match = $regex.Match($text, $offset)
        Require $match.Success 'The settings JSON cannot be tokenized without changing its encoding.'
        $group = $match.Groups['token']
        $tokens.Add(@{raw=$group.Value;start=$group.Index;length=$group.Length})
        $offset = $match.Index + $match.Length
    }
    $spans = @{}
    $index = 0
    ReadJsonValue $tokens ([ref]$index) '' $spans
    Require ($index -eq $tokens.Count -and $spans.Count -eq 3) 'Exactly three existing Main model string fields are required.'
    return $spans
}

function UnrelatedDigest([string]$text) {
    $data = ConvertFrom-Json -InputObject $text
    foreach ($key in $mainKeys) { $data.llm.PSObject.Properties.Remove($key) }
    return Digest ($utf8.GetBytes(($data | ConvertTo-Json -Depth 100 -Compress)))
}

function InstalledArtifact([string]$path, $entries, [string]$modelsDirectory) {
    $resolved = (Resolve-Path -LiteralPath $path).ProviderPath
    Require ($resolved.StartsWith($modelsDirectory + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) 'Select an already installed artifact under this installation\Models.'
    Require (Test-Path -LiteralPath $resolved -PathType Leaf) 'The selected installed artifact is not a file.'
    $name = [IO.Path]::GetFileName($resolved)
    Require ([IO.Path]::GetDirectoryName($resolved).Equals($modelsDirectory, [StringComparison]::OrdinalIgnoreCase)) 'Select an installed artifact directly inside Models.'
    $entry = @($entries | Where-Object { $(if ($_.localFile) { $_.localFile } else { $_.file }) -ceq $name })
    Require ($entry.Count -eq 1) 'The installed artifact must have exactly one matching manifest entry.'
    Require (-not [string]::IsNullOrWhiteSpace([string]$entry[0].id) -and @($entries | Where-Object { $_.id -ceq $entry[0].id }).Count -eq 1) 'The selected manifest artifact id must be nonempty and unique.'
    Require ([string]$entry[0].sha256 -cmatch '^[0-9a-f]{64}$') 'The manifest artifact has no valid pinned SHA-256.'
    $size = (Get-Item -LiteralPath $resolved).Length
    Require ($size -gt 0 -and $size -eq [long]$entry[0].bytes) 'Installed artifact size does not match the manifest.'
    $hash = (Get-FileHash -LiteralPath $resolved -Algorithm SHA256).Hash.ToLowerInvariant()
    Require ($hash -ceq [string]$entry[0].sha256 -and (Get-Item -LiteralPath $resolved).Length -eq $size) 'Installed artifact SHA-256 does not match the manifest.'
    return @{path=$resolved;file=$name;id=[string]$entry[0].id;projectorFor=$entry[0].projectorFor;bytes=$size;sha256=$hash}
}

Require (($Mode -eq 'Apply') -eq (-not [string]::IsNullOrEmpty($ExpectedDryRunReceipt))) 'Apply requires its explicit earlier DryRun receipt; DryRun takes none.'
$settings = (Resolve-Path -LiteralPath $SettingsPath).ProviderPath
$manifest = (Resolve-Path -LiteralPath $ManifestPath).ProviderPath
$receipt = [IO.Path]::GetFullPath($ReceiptPath)
Require (-not (Test-Path -LiteralPath $receipt)) 'Existing receipts are never replaced.'
Require (Test-Path -LiteralPath ([IO.Path]::GetDirectoryName($receipt)) -PathType Container) 'Create the explicit receipt directory first.'
$install = Split-Path -Parent (Split-Path -Parent $settings)
$models = (Resolve-Path -LiteralPath (Join-Path $install 'Models')).ProviderPath.TrimEnd('\', '/')
$overlay = if ($OwnerOverlayPath) { [IO.Path]::GetFullPath($OwnerOverlayPath) } else { Join-Path $install 'RuntimeData\Settings\owner-settings.json' }
$original = ReadBounded $settings
$source = Text $original
$data = ConvertFrom-Json -InputObject $source
$spans = MainStringSpans $source
Require ([string]$data.activeProfile -cmatch '^[a-zA-Z0-9_-]+$') 'The selected active profile id is unsupported.'
$profile = (Resolve-Path -LiteralPath (Join-Path (Split-Path -Parent $settings) ('Profiles\' + $data.activeProfile + '.json'))).ProviderPath
$manifestBytes = ReadBounded $manifest
$manifestData = ConvertFrom-Json -InputObject (Text $manifestBytes)
$model = InstalledArtifact $ModelPath $manifestData.artifacts $models
$projector = InstalledArtifact $ProjectorPath $manifestData.artifacts $models
Require ([string]::IsNullOrEmpty($model.projectorFor) -and $projector.projectorFor -ceq $model.id -and $model.id -cne $projector.id -and $model.path -cne $projector.path) 'The manifest does not identify this projector as matching the selected model.'
$requested = @{modelName=$model.file;modelPath=('Models/' + $model.file);multimodalProjectorPath=('Models/' + $projector.file)}
$proposed = $source
foreach ($key in ($mainKeys | Sort-Object { $spans[$_].start } -Descending)) {
    $span = $spans[$key]
    $replacement = ConvertTo-Json -InputObject $requested[$key] -Compress
    $proposed = $proposed.Remove($span.start, $span.length).Insert($span.start, $replacement)
}
$proposalBytes = $utf8.GetBytes($proposed)
if ($original.Length -ge 3 -and $original[0] -eq 0xef -and $original[1] -eq 0xbb -and $original[2] -eq 0xbf) {
    $proposalBytes = [byte[]]([byte[]](0xef, 0xbb, 0xbf) + $proposalBytes)
}
$afterData = ConvertFrom-Json -InputObject $proposed
foreach ($key in $mainKeys) { Require ($afterData.llm.$key -ceq $requested[$key]) 'The prepared Main model setting does not match the requested value.' }
$unrelatedBefore = UnrelatedDigest $source
Require ($unrelatedBefore -ceq (UnrelatedDigest $proposed)) 'An unrelated JSON value changed.'
$before = @{
    settingsPath=$settings;settingsSha256=(Digest $original);profile=(FileIdentity $profile);overlay=(FileIdentity $overlay)
    manifest=(FileIdentity $manifest);unrelatedJsonSha256=$unrelatedBefore
    main=@{modelName=$data.llm.modelName;modelPath=$data.llm.modelPath;multimodalProjectorPath=$data.llm.multimodalProjectorPath;visionEnabled=$data.llm.visionEnabled}
}
$report = @{
    schemaVersion=1;kind='installed-main-model-selection';mode=$Mode;before=$before;requestedMain=$requested
    assets=@{model=$model;projector=$projector};proposedSettingsSha256=(Digest $proposalBytes)
    unrelatedJsonValuesPreserved=$true;unrelatedByteSpansPreserved=$true;restartRequired=$true
    visionSettingChanged=$false;normalSessionStarted=$false;modelServersStarted=$false
}
if ($Mode -eq 'DryRun') {
    $report.originalBackupPath = $receipt + '.original-settings.bin'
    $report.proposedSettingsPath = $receipt + '.proposed-settings.json'
    WriteNew $report.originalBackupPath $original
    WriteNew $report.proposedSettingsPath $proposalBytes
    $report.wouldChangeSettings = $report.before.settingsSha256 -cne $report.proposedSettingsSha256
}
else {
    $expected = ConvertFrom-Json -InputObject (Text (ReadBounded ((Resolve-Path -LiteralPath $ExpectedDryRunReceipt).ProviderPath)))
    Require ($expected.kind -ceq $report.kind -and $expected.mode -ceq 'DryRun' -and $expected.schemaVersion -eq 1) 'The earlier receipt is not a supported DryRun.'
    foreach ($field in @('settingsPath', 'settingsSha256', 'unrelatedJsonSha256')) { Require ($expected.before.$field -ceq $before[$field]) 'Settings changed after DryRun.' }
    foreach ($field in @('profile', 'overlay', 'manifest')) {
        foreach ($key in @('path', 'exists', 'sha256')) { Require ($expected.before.$field.$key -ceq $before[$field][$key]) 'The profile, owner choices or manifest changed after DryRun.' }
    }
    foreach ($key in $mainKeys) { Require ($expected.requestedMain.$key -ceq $requested[$key]) 'Apply selects a different Main model than DryRun.' }
    foreach ($asset in @('model', 'projector')) {
        foreach ($key in @('path', 'sha256', 'bytes')) { Require ($expected.assets.$asset.$key -ceq $report.assets[$asset][$key]) 'An installed artifact changed after DryRun.' }
    }
    Require ($expected.proposedSettingsSha256 -ceq $report.proposedSettingsSha256) 'The reviewed proposal differs from the freshly prepared change.'
    Require ((Digest (ReadBounded $expected.originalBackupPath)) -ceq $before.settingsSha256) 'The exact original-byte backup is unavailable.'
    Require ((Digest (ReadBounded $expected.proposedSettingsPath)) -ceq $report.proposedSettingsSha256) 'The reviewed proposed settings are unavailable.'
    $temp = $settings + '.main-selection-' + [Guid]::NewGuid().ToString('N') + '.tmp'
    $atomicBackup = $receipt + '.pre-apply-settings.bin'
    Require (-not (Test-Path -LiteralPath $atomicBackup)) 'An existing original-byte backup is never replaced.'
    try {
        WriteNew $temp $proposalBytes
        Require ((Digest (ReadBounded $settings)) -ceq $before.settingsSha256) 'Settings changed while Apply was preparing; nothing was replaced.'
        [IO.File]::Replace($temp, $settings, $atomicBackup)
    }
    finally { if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp } }
    Require ((Digest (ReadBounded $atomicBackup)) -ceq $before.settingsSha256) 'A concurrent writer changed settings at replacement; inspect the retained pre-apply backup before continuing.'
    Require ((Digest (ReadBounded $settings)) -ceq $report.proposedSettingsSha256) 'The installed settings no longer match the reviewed bytes; the original backup was retained.'
    foreach ($field in @('profile', 'overlay', 'manifest')) {
        $current = FileIdentity $before[$field].path
        Require ($current.sha256 -ceq $before[$field].sha256) 'An external writer changed an unrelated file during Apply; inspect the retained receipt and backup.'
    }
    $report.originalBackupPath = $expected.originalBackupPath
    $report.atomicBackupPath = $atomicBackup
    $report.proposedSettingsPath = $expected.proposedSettingsPath
    $report.expectedDryRunReceipt = (Resolve-Path -LiteralPath $ExpectedDryRunReceipt).ProviderPath
    $report.afterSettingsSha256 = Digest (ReadBounded $settings)
}
WriteReceipt $receipt $report
Write-Output ($Mode + ' retained: ' + $receipt + '. Main vision is unchanged; use the existing supported vision-setting helper separately.')
