# Structured answer delivery: 2026-10-07

Implementation source is `d3c21b10967617230a3cc11e2d5f880324f142b7`.
This delivery fixes requested JSON preservation, bounded current-turn format
guidance and honest answer diagnostics. It does not complete the remaining
upgrade roadmap or qualify general reasoning, autonomy or personality.
See [ownership and verification design](ANSWER_QUALITY_STRUCTURED_REPLIES.md).

## Verified implementation

The affected 14 executable targets built with Qt 6.8.3 and matching MinGW 13.1,
with assertions enabled. All **15 selected native checks passed**: 13 focused
checks, desktop startup and the broad foundation suite. Combined wall time for
those three selections was 174.03 seconds. The preserved desktop also passed an
offscreen `--ui-smoke-test` with exit 0.

The native regressions verify actual provider request contracts, literal data
preservation, current-turn intent, arrays, quotes, withdrawal and negation,
incomplete/truncated output refusal, all seven disallowed control tokens,
normalized report admission, unchanged strict oracle tokens, ordinary chat
settings, authority, audience privacy and response timing. A 250,000-newline
request reproduced a native stack overflow before correction and passed after
whitespace compaction. Intermediate file-lock attempts are retained alongside
the final successful build and test receipts.

Independent source review accepted the final bounded patch with no remaining
material P1/P2 finding. The authored profile SHA-256 remains
`c3e07fcf842adefccc136bd4240ba72db0fcec60a9741ba86464da1fe320411d`;
its temperature remains 0.75. Ordinary chat retains its previous guidance and
sampling policy. Configuration preservation does not prove personality quality.

## Fresh local-model validation

Campaign `answer-quality-d3c21b1-20261007` used the existing Qwen3.5-4B Q4_K_M
weights, owned loopback llama.cpp provider, 8192 context, unchanged authored
profile and seeds 11, 29 and 47. The independent 24-case set was frozen before
outcomes and contains four multi-turn cases: **72 expected case/seed slots** and
87 actual generation requests. Exact corpus SHA-256 is recorded in the design.

| Seed | Strict answer-and-JSON matches | Slots |
|---|---:|---:|
| 11 | 22 | 24 |
| 29 | 21 | 24 |
| 47 | 21 | 24 |
| Total | **64 (88.89%)** | **72** |

All 72 slots were recorded, available, admitted and mechanically successful.
The strict outcomes include **four shape mismatches and four value mismatches**,
with zero invalid JSON, missing samples, unavailable samples or invalid bindings.
Raw and delivered categories agree; zero repair-introduced or repair-rescued
failures were observed. The strict `json-exact-v1` oracle and denominators remain
unchanged. A syntactically valid reply still fails for incorrect keys or values.

Independent evidence review verified all 144 source/output references, 72
task/stamp/scope bindings and 72 campaign-manifest bindings. Source, native binary,
provider, profile and corpus identities matched before and after collection;
source was clean and the wrapper verified its build. All 87 forwarded requests
carried the declared seed. The 72 format contracts contained only a generic
object/array type; no oracle expected answer was supplied to generation.

Remaining failures are substantive: timer arithmetic returned 15, 155 and 145
instead of 125 seconds; one disks calculation returned 37 instead of 47.
Other failures added, renamed or nested fields despite retaining supplied facts.
These are priorities for further reasoning and task/schema adherence work.
No production tuning followed fresh validation outcomes.

Wrapper and runner exited 0 with stable provenance and no mismatches. Backend
seed behavior and external provider qualification remain unverified. All 72
personality verdicts and independent human review remain pending;
`liveQualified=false`. This narrow result is not comparable to the different
original 720-slot population, whose **184/720** exploratory score and receipts
remain untouched.

## Development and ordinary conversation controls

The four-case, unseeded development/calibration comparison produced no valid JSON
with the preserved prior executable; the candidate produced four valid JSON
replies and two exact matches. The two other replies used incorrect key names.
This small combined-change comparison cannot establish a statistical accuracy
gain or attribute a gain to grammar, guidance or DRY separately.

The separate six ordinary conversation cases contain eight turns. The preserved
prior executable passed five cases mechanically; the candidate passed four.
The candidate exceeded a sentence ceiling twice and one greeting made an
unnecessary voice-status excuse. This unseeded comparison is not a personality
grade, and it does not establish a subjective improvement or preservation.
Authored personality, private-memory scope and ordinary review preferences were
not changed. Physical speech, hearing and lip sync were not exercised here.

## Retained evidence and build

Evidence stays outside Git at
`C:/Users/davis/.codex/attachments/revia-answer-quality-2026-10-07`.
The actual campaign is in `fresh-d3c21b1`, including before/after receipts,
raw/final outputs, requests, manifests and `evidence-audit-verified.json`.
The initial audit assumed compact manifest serialization; its corrected audit
verified the actual sorted-key `dump(2)` representation. Both audits and the
original campaign receipts remain retained.

Native logs, JUnit, regression RED/GREEN receipts, executable hashes and deployed
smoke receipts are retained under `native-verification` and the independent
pipeline evidence directory. The runnable desktop and live evaluator were
preserved at `build/answer-quality-d3c21b1` in the owner's primary checkout,
with matching Qt dependencies, Config and Tools. Existing owner edits and user
data are excluded from this delivery.
