# Foundation and cognition evaluation

This package implements FND-01, FND-02, FND-03 and COG-01: task/evidence contracts,
durable receipts, reproducible campaigns and a structured cognition baseline.
Broad intelligence, personality, games and autonomy remain outside this baseline; corpus sizes describe its design.

## Contracts and journal ownership

`Public/Core/taskContract.h` carries goal, constraints, deliverables, acceptance
obligations, existing resource ceilings, cancellation lineage, `RuntimeStamp` and
`MemoryScope`. Turn, node and action adapters reuse those owners. Validation checks
schema, captured/current identity, scope and cancellation; a contract grants no
authority. `EvidenceBundle` exposes validated immutable references. A content digest
binds bytes and provenance; it does not prove that the referenced claim is true.

`EvidenceJournal` owns persisted intent/result receipts and evidence projections;
`ActionAuditLogger` adapts existing readable action records to that same owner.
An effect requires a durable intent receipt before dispatch. A missing durable
result after dispatch leaves an unresolved effect requiring reconciliation.
Changing attempt/policy version does not hide unresolved effects for the same task/scope.
Recovery quarantines damage and reports health; it never replays an executor,
grants authority or infers that an effect succeeded.

Lifecycle observations use a bounded telemetry queue with a separate short lock.
A serialized flusher drains a batch before disk I/O; failed writes restore
remaining observations within the bound. Health reports pending and durable drop
counts separately. Telemetry cannot substitute for a durable intent/result pair.
The runtime bridge records content-omitted state/component/timing/warning metadata
under the explicit `runtime-system` unknown audience and captured companion stamp.
This scope does not assign private content to a participant or guess an audience.

## Campaign commands and retained provenance

Run these commands from the source root. Set `$buildDirectory` to an already
configured absolute build directory whose `CMakeCache.txt` names this source tree.
Each invocation needs a fresh campaign ID and a nonexistent output directory.

```powershell
$buildDirectory = 'C:\ReviaBuilds\debug' # Replace with the configured absolute directory.
$evidenceRoot = Join-Path $env:LOCALAPPDATA 'ReviaEvaluation'
foreach ($changeId in 'FND-01', 'FND-02', 'FND-03', 'COG-01') {
    $campaignId = "$changeId-$([guid]::NewGuid().ToString('N'))"
    & .\Tools\RunArchitectureEvaluation.ps1 -Preset debug -ChangeId $changeId `
        -BuildDirectory $buildDirectory -CampaignId $campaignId `
        -OutputDirectory (Join-Path $evidenceRoot $campaignId)
    if ($LASTEXITCODE -ne 0) { throw "Campaign failed: $campaignId" }
}
```

The registry in `Config/Evaluation/architecture_tests.json` names nine exact test
IDs. The runner resolves them from actual `ctest --show-only=json-v1` labels,
verifies source ownership, reconfigures and rebuilds selected native targets,
then retains JUnit and raw output with `--no-tests=error`. Missing, removed,
disabled, skipped or unexecuted selections cannot count as completed coverage.
`-FixtureOnly` permits a disposable CTest project and records unqualified evidence.
A fixture label, including a 720-slot fixture, cannot fulfill live work.

The immutable manifest pins commit plus relevant dirty patch/untracked identity,
build and selected artifact digests, supplied provider/model artifact digests,
settings, fixtures, oracle version, hardware, declared seed and timing boundary.
Dirty source remains explicitly dirty. `-ProviderFiles` supplies actual provider
and model files; absent files do not establish provider availability. Campaign
`REVIA_EVALUATION_SEED` is a declared seed mechanism, not proof of backend use.
Manifest mismatches invalidate comparisons. Raw results retain pending integration,
live and independent human evidence requirements even when fixtures pass.

## Structured cognition and live collection

The heldout corpus has 12 families, 240 unique episodes and seeds 11, 29 and 47:
720 expected slots. Development, calibration and heldout partitions are checked
for source/entity overlap. `json-exact-v1` compares strict JSON with independently
authored expected answers. Mechanical checks, oracle decisions, unavailable or
unbound outputs, and human semantic/personality judgments have separate counters.
Live samples admit typed campaign/task contracts and immutable source/output bundles;
mixed build/provider cohorts cannot count. Human reviews also bind campaign identity.

With an owned loopback provider running, collect the optional mode with receipts:

```powershell
$campaignId = "cognition-$([guid]::NewGuid().ToString('N'))"
$profile = 'C:\path\to\authored-profile.json' # Use the existing authored profile.
$corpus = (Resolve-Path '.\Tests\Fixtures\Cognition\heldout-manifest.json').Path
& .\Tools\RunCognitionLiveCampaign.ps1 -BuildDirectory $buildDirectory -CampaignId $campaignId `
    -OutputDirectory (Join-Path $evidenceRoot $campaignId) -ProfilePath $profile -CorpusPath $corpus `
    -Port 8080 -LaunchMetadataPath 'C:\path\to\owned-launch-receipt.json'
```

The mode retains authored prompt/profile settings and the existing runtime/evaluator.
It captures original corpus bytes and observes `/v1/models` and `/props`;
a single observed model ID selects completions; each seed enters retained forwarded requests.
`providerIdentityVerified`, `backendSeedVerified` and `liveQualified` remain false.
Qualification needs an external receipt binding source, build, provider executable
and actual weights digests, backend seed evidence and independent bound reviews.

Per-case source/output/task/bundle/raw/review/traffic, per-seed manifests and aggregate
are retained once; reruns cannot replace them. `.progress.json` is disposable.
Runner failures retain unavailable slots; human semantic/personality verdicts remain pending.

To roll back evaluation exposure, disable the added registry/CTest labels and stop using
`cognition`. Preserve evidence and journal data; the legacy evaluator/default/review modes remain.
