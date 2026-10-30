$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$tool = Join-Path $repo 'Tools\SelectInstalledMainModel.ps1'
$root = Join-Path $repo ('build\stage-b-20261029\main-selection-test-' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory((Join-Path $root 'Config\Profiles'))
[void][IO.Directory]::CreateDirectory((Join-Path $root 'Models'))
[void][IO.Directory]::CreateDirectory((Join-Path $root 'RuntimeData\Settings'))
$utf8 = [Text.UTF8Encoding]::new($false, $true)

function Check([bool]$value, [string]$reason) {
    if (-not $value) { throw $reason }
}

function WriteUtf8([string]$path, [string]$text) {
    [IO.File]::WriteAllBytes($path, $utf8.GetBytes($text))
}

function Hash([string]$path) {
    return (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Refused([scriptblock]$action, [string]$reason) {
    $failed = $false
    try { & $action | Out-Null } catch { $failed = $true }
    Check $failed $reason
}

$model = Join-Path $root 'Models\candidate.gguf'
$projector = Join-Path $root 'Models\candidate-projector.gguf'
WriteUtf8 $model 'GGUF disposable model bytes, never executed'
WriteUtf8 $projector 'GGUF disposable projector bytes, never executed'
$manifest = Join-Path $root 'Config\model_manifest.json'
$entries = @(
    @{id='fixture';file='candidate.gguf';sha256=(Hash $model);bytes=(Get-Item -LiteralPath $model).Length;projectorFor=$null},
    @{id='fixture-vision';file='candidate-projector.gguf';sha256=(Hash $projector);bytes=(Get-Item -LiteralPath $projector).Length;projectorFor='fixture'}
)
WriteUtf8 $manifest (@{artifacts=$entries} | ConvertTo-Json -Depth 8)
$settings = Join-Path $root 'Config\settings.json'
$profile = Join-Path $root 'Config\Profiles\revia.json'
$overlay = Join-Path $root 'RuntimeData\Settings\owner-settings.json'
WriteUtf8 $profile '{"id":"revia","systemPrompt":"Authored identity stays exact.","temperature":0.75,"unknownAuthored":{"keep":"all"}}'
WriteUtf8 $overlay '{"schemaVersion":1,"values":{"vision.enabled":false,"llm.imageMinTokens":"1024","intelligence.expert.enabled":false}}'
$template = @'
{
  "activeProfile": "revia",
  "unchanged": "old-main.gguf, modelPath, <UNICODE>",
  "llm": {
    "modelName" : <NAME>, "modelPath" : <MODEL>,
    "multimodalProjectorPath" : <PROJECTOR>,
    "visionEnabled": false, "temperature": 0.8, "contextSize": 32768,
    "unknown": {"keep": [1, true, null, {"nested": "modelPath"}]}
  },
  "intelligence": {"fast":{"enabled":true},"expert":{"modelName":"old-main.gguf","modelPath":"Models/old-main.gguf","visionEnabled":false}},
  "vision": {"enabled":false,"continuousAwareness":false}
}
'@
$template = $template.Replace('<UNICODE>', ('caf' + [char]0xe9 + ', ' + [char]0x72d7 + '.png'))
$originalText = $template.Replace('<NAME>', '"old-main.gguf"').Replace('<MODEL>', '"Models/old-main.gguf"').Replace('<PROJECTOR>', '"Models/old-projector.gguf"').Replace("`n", "`r`n")
$expectedText = $template.Replace('<NAME>', '"candidate.gguf"').Replace('<MODEL>', '"Models/candidate.gguf"').Replace('<PROJECTOR>', '"Models/candidate-projector.gguf"').Replace("`n", "`r`n")
$bom = [byte[]](0xef, 0xbb, 0xbf)
[IO.File]::WriteAllBytes($settings, [byte[]]($bom + $utf8.GetBytes($originalText)))
$originalHash = Hash $settings
$profileHash = Hash $profile
$overlayHash = Hash $overlay
$common = @{SettingsPath=$settings; ManifestPath=$manifest; ModelPath=$model; ProjectorPath=$projector; OwnerOverlayPath=$overlay}
$dry = Join-Path $root 'dry.json'
& $tool @common -Mode DryRun -ReceiptPath $dry | Out-Null
$plan = Get-Content -LiteralPath $dry -Raw | ConvertFrom-Json
Check ((Hash $settings) -eq $originalHash) 'DryRun wrote normal settings.'
Check ((Hash $plan.originalBackupPath) -eq $originalHash) 'Original-byte backup differs.'
$expectedBytes = [byte[]]($bom + $utf8.GetBytes($expectedText))
Check ([Convert]::ToBase64String([IO.File]::ReadAllBytes($plan.proposedSettingsPath)) -eq [Convert]::ToBase64String($expectedBytes)) 'Proposal changed bytes outside the three Main string values.'
Check ($plan.unrelatedByteSpansPreserved -and $plan.unrelatedJsonValuesPreserved) 'Preservation was not established.'
Check ($plan.requestedMain.modelName -eq 'candidate.gguf' -and -not $plan.before.main.visionEnabled) 'Proposal changed vision or chose the wrong model.'
Refused { & $tool @common -Mode Apply -ReceiptPath (Join-Path $root 'missing-dry.json') } 'Apply accepted no earlier dry-run receipt.'
Check ((Hash $settings) -eq $originalHash) 'A refused Apply changed settings.'
WriteUtf8 $profile '{"id":"revia","temperature":0.1}'
Refused { & $tool @common -Mode Apply -ReceiptPath (Join-Path $root 'stale.json') -ExpectedDryRunReceipt $dry } 'Apply accepted a changed authored profile.'
Check ((Hash $settings) -eq $originalHash) 'The stale-profile refusal changed settings.'
WriteUtf8 $profile '{"id":"revia","systemPrompt":"Authored identity stays exact.","temperature":0.75,"unknownAuthored":{"keep":"all"}}'
$overlayBytes = [IO.File]::ReadAllBytes($overlay)
WriteUtf8 $overlay '{"schemaVersion":1,"values":{"vision.enabled":true}}'
Refused { & $tool @common -Mode Apply -ReceiptPath (Join-Path $root 'stale-overlay.json') -ExpectedDryRunReceipt $dry } 'Apply accepted changed unrelated owner choices.'
Check ((Hash $settings) -eq $originalHash) 'The stale-overlay refusal changed settings.'
[IO.File]::WriteAllBytes($overlay, $overlayBytes)
$settingsBytes = [IO.File]::ReadAllBytes($settings)
[IO.File]::AppendAllText($settings, " `r`n", $utf8)
$changedSettingsHash = Hash $settings
Refused { & $tool @common -Mode Apply -ReceiptPath (Join-Path $root 'stale-settings.json') -ExpectedDryRunReceipt $dry } 'Apply accepted changed settings bytes.'
Check ((Hash $settings) -eq $changedSettingsHash) 'The stale-settings refusal overwrote the changed bytes.'
[IO.File]::WriteAllBytes($settings, $settingsBytes)
$proposalBytes = [IO.File]::ReadAllBytes($plan.proposedSettingsPath)
[IO.File]::AppendAllText($plan.proposedSettingsPath, ' ', $utf8)
Refused { & $tool @common -Mode Apply -ReceiptPath (Join-Path $root 'stale-proposal.json') -ExpectedDryRunReceipt $dry } 'Apply accepted a modified reviewed proposal.'
Check ((Hash $settings) -eq $originalHash) 'The stale-proposal refusal changed settings.'
[IO.File]::WriteAllBytes($plan.proposedSettingsPath, $proposalBytes)
& $tool @common -Mode Apply -ReceiptPath (Join-Path $root 'apply.json') -ExpectedDryRunReceipt $dry | Out-Null
$appliedReceipt = Get-Content -LiteralPath (Join-Path $root 'apply.json') -Raw | ConvertFrom-Json
Check ((Hash $appliedReceipt.atomicBackupPath) -eq $originalHash) 'Atomic replacement did not retain the exact original bytes.'
Check ([Convert]::ToBase64String([IO.File]::ReadAllBytes($settings)) -eq [Convert]::ToBase64String($expectedBytes)) 'Apply did not install the exact reviewed proposal.'
Check ((Hash $profile) -eq $profileHash -and (Hash $overlay) -eq $overlayHash) 'Profile or owner choices changed.'
$applied = Get-Content -LiteralPath $settings -Raw | ConvertFrom-Json
Check (-not $applied.llm.visionEnabled -and -not $applied.vision.enabled -and -not $applied.vision.continuousAwareness) 'Model selection enabled a vision or screen setting.'
Check ($applied.intelligence.expert.modelName -eq 'old-main.gguf') 'Model selection changed another model tier.'
Refused { & $tool @common -Mode DryRun -ReceiptPath $dry } 'An earlier receipt was overwritten.'
WriteUtf8 $model 'tampered'
Refused { & $tool @common -Mode DryRun -ReceiptPath (Join-Path $root 'bad-sha.json') } 'An unverified installed artifact was accepted.'
WriteUtf8 $model 'GGUF disposable model bytes, never executed'
$entries[1].projectorFor = 'different-model'
WriteUtf8 $manifest (@{artifacts=$entries} | ConvertTo-Json -Depth 8)
Refused { & $tool @common -Mode DryRun -ReceiptPath (Join-Path $root 'bad-pair.json') } 'A mismatched manifest projector pair was accepted.'
$entries[1].projectorFor = 'fixture'
WriteUtf8 $manifest (@{artifacts=$entries} | ConvertTo-Json -Depth 8)
WriteUtf8 $settings '{"activeProfile":"revia","llm":{"modelName":"a","modelName":"b","modelPath":"a","multimodalProjectorPath":"b"}}'
Refused { & $tool @common -Mode DryRun -ReceiptPath (Join-Path $root 'duplicate.json') } 'Duplicate JSON properties were accepted.'
WriteUtf8 $settings '{"activeProfile":"revia","llm":{"modelName":"a","modelPath":3,"multimodalProjectorPath":"b"}}'
Refused { & $tool @common -Mode DryRun -ReceiptPath (Join-Path $root 'wrong-type.json') } 'A nonstring Main path was accepted.'
Write-Output ('PASS installed-Main selection, preservation, stale snapshot, hash/pair and ambiguity checks. Artifacts: ' + $root)
