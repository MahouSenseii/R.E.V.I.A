# Structured answer quality: 2026-10-07

This continuation addresses verified corruption of requested JSON replies and
separates formatting failures from incorrect answers. It does not replace the
authored Revia profile or qualify general intelligence, autonomy or personality.

## Connected ownership

`Agents/replyFormat` owns a stateless, bounded interpretation of the current
user's explicit output request and validation of complete JSON containers.
Quoted examples, explanatory mentions and negated requests do not enable the
format contract. A later ordinary turn uses ordinary conversation guidance.
Ambiguous instructions conservatively retain conversation mode. The detector
handles objects and arrays; it does not promise arbitrary natural-language
schema extraction or scalar JSON replies.

`ConversationStylePolicy` adds transient format guidance for that turn. The
llama.cpp provider sends an object/array grammar without any oracle expectation,
retains the profile and its temperature, and buffers structured output until
complete validation. Structured output omits conversational lexical stop markers,
inline reasoning and DRY repetition penalties. These choices protect exact data;
no isolated causal improvement from DRY tuning is claimed. Ordinary conversation
keeps its existing settings and presentation pipeline.

The style policy and hard filter preserve validated JSON values instead of
collapsing spaces, repeated sentences or literal vocalization markers. Existing
prompt-leak and unsupported capability guards remain active. Disallowed control
tokens in JSON cause an explicit blocked refusal rather than silently edited
data. Malformed, duplicate-key, excessive-depth, oversized, wrong-container or
token-truncated structured output is unsuccessful and emits no partial delta.
Cancellation, memory scope and action authority retain their existing owners.

`Evaluation/cognitionEvaluation` adds raw/final diagnostics: invalid JSON,
shape mismatch, value mismatch, exact match or unjudged. Value comparison is
judged only when valid JSON has the expected shape. Repair counters distinguish
a correct raw answer damaged downstream from an answer rescued downstream.
The original `json-exact-v1` oracle and all denominators remain unchanged.
Campaign admission normalizes claims before both aggregation and serialization;
rejected or unavailable samples cannot carry accepted answer/personality claims.

## Director and workers

| Role | Assigned area | Status |
|---|---|---|
| Director / integration | Provider contract, bounded intent detector, native build, live evidence and main integration | Native checks passed; fresh validation and integration pending |
| Pipeline worker | Shared style/hard-filter preservation and security regression checks | Native regression and integrated checks passed |
| Evaluation worker | Honest raw/final diagnostics and authoritative report serialization | Native regression and integrated checks passed |
| Independent validation author | New 24-case / 72-slot set and six personality control conversations | Frozen before outcomes |
| Independent reviewer | Source review of format, security, admission and ordinary conversation paths | Accepted; no remaining material source finding |

## Verification design

Regression tests exercise production code with a loopback completion transport,
not inferred model behavior. They cover current-turn intent, quoted and negated
requests, arrays, literal data, malformed/truncated output, profile preservation,
security refusal and reversion to ordinary conversation. Large space/newline and
modifier inputs test detector resource limits. Existing authority, audience,
answer-quality, cognition, latency and desktop checks remain part of integration.

The affected 14 executable targets built with Qt 6.8.3 and matching MinGW 13.1,
with assertions enabled. All 15 selected checks passed: 13 focused checks,
desktop startup, and the broad foundation suite. A 250,000-newline request
reproduced a native stack overflow before compaction and passed after correction.

The small development/calibration comparison reuses four development cases with
the preserved previous executable and the changed executable against the same
owned local provider. It is unseeded and exploratory. It cannot establish a
statistical gain or isolate one changed component.

The independent fresh set contains 24 unique cases, four with multiple turns,
and seeds 11, 29 and 47: 72 expected slots. It was frozen before model outcomes.
Corpus SHA-256 is
`cc10bf3d3d6f3f5656e022a06355c93aaaa0e5470653f9655cbafe965f98b71c`.
The six personality conversations contain eight turns; their corpus SHA-256 is
`3c7c1e2c2665f0d52bd1cd0976d625a203db6827cc98c4579297b3cf0047ef55`.
Mechanical control checks do not establish personality quality. Human semantic
and personality review and external qualification remain pending.

Original 720-run baseline evidence and its 184/720 score remain unchanged.
New evidence stays outside Git under the owner's Codex attachment directory;
an acceptance receipt will record source, build, checks, actual outcomes and
limitations after verification.
