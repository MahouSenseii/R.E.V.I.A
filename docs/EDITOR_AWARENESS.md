# Saved-file editor observations

The first editor integration reports one explicitly selected saved C/C++ file.
It does not read unsaved editor buffers, follow the cursor, take screenshots,
execute commands, generate a reply through the HTTP endpoint, or establish that
code builds or passes tests. Authorship is unknown. The existing session decides
whether an admitted observation merits a later, bounded comment.

## Validation status and moving to another computer

The saved-file opening now uses conservative native C++ checks for a declared
source subset. Twelve real-watcher trials on the RTX 5070 on 2026-10-29 produced
seven correct source-specific comments and follow-ups, plus five explicit
no-supported-diagnostic silences. These include renamed functions and variables,
another vector element type, checked and unchecked access, local references,
corrected code, a boundary guard, hostile source instructions and unsupported
division. The seven comments took 5.229–5.358 s from actual save, including normal
watcher debounce and attention admission; follow-ups took 0.082–0.098 s. The
four-second attention stability interval remains in force.

These results qualify the declared native checks, not arbitrary C++ correctness
or general model code-review accuracy. An empty check result makes no safety
claim. The normal model and authored profile remain unchanged. One ordinary
story control falsely denied creative-writing ability; a captured repeat and
six bounded paired provider controls supplied proper stories. Both prompt
variants passed those pairs, so the capability-scope clarification is not claimed
as a demonstrated cause of improvement. A later controlled tool-workflow pair
reproduced the denial at the same seed and supplied a proper story through plain
model generation. Bounded self-contained composition now skips irrelevant tool
definitions and extra source context, while attachment, source, reminder and other
dependent requests retain their existing paths. Sixty-four intent cases and the
runtime boundaries have separate regression coverage.

Fresh final bounds/lifetime confirmations took 5.217–5.274 s from save and
68–77 ms for their factual follow-ups. All six final ordinary story, poem and
opinion controls produced answers without false capability denials at unchanged
temperature 0.75. One opinion chose a third alternative; general personality and
creative quality remain unqualified. Final delivery-regression verification
and these ordinary-conversation controls are recorded in the
[continuation record](COMPANION_UPGRADE_PROGRESS.md). Passing tests or receiving
HTTP 202 alone does not establish useful code review.

The owner-approved 5.63 GB Qwen3.5-9B Q4_K_M candidate was subsequently downloaded
and checksum-verified. Ten real native trials, including a faster zero-thinking
comparison, did not qualify it: bounds explanations still confused checked
exceptions with undefined behavior/crashes, and one safe assessment was
suppressed for an invalid none contract. Three other safe controls produced
appropriate native silence. Default candidate comments took about 16.7–17.4 s
from save; the 8.45 s zero-thinking trial remained incorrect. The normal model
was not switched. Paired grammar removal, facts-first and actual-evidence-fed
offline prototypes also failed semantic checks and were not integrated. The
native boundary described below replaces model-generated factual deductions for
the supported optional saved-file comments. Historical model failures remain
retained as evidence; the installed 9B candidate was not selected.

An earlier live preflight used an RTX 3070 Laptop GPU with 8 GiB VRAM and 16 GiB RAM.
Windows reported other microphone use, and available RAM was about 1.4 GiB. The
owner chose to record that test limit. No attention bypass or final model trial
was performed. The newer text-only RTX 5070 trials do not qualify physical voice,
video, or generation-time conversation performance. The retained image provider's
independent retest has its own evidence and limits in the continuation record.

On another computer, pull main and build with `Tools/Build.ps1`. Verify that
machine's actual model paths, hardware headroom and permissions before enabling
awareness. Models, personal memories, runtime settings and credentials are local
data and are not transferred by a Git pull. Use a private transfer for personal
data. A Windows DPAPI token belongs to its original user/machine; let the existing
local API owner provision a token on the destination instead of copying or
exporting the old token as plaintext.

The next live checks should qualify repeatable source judgment and faithful
natural presentation with an authorized local model, including false silence,
generation exhaustion and safe/instruction negatives, then live Focus and
physical interruption. The downloaded 9B candidate remains unselected. Run
with an idle microphone and measured memory headroom. Inspect actual model output before accepting Stage A;
retain unsuccessful trials.

## Setup

Use an installation built with the saved-file observation adapter. The local API
ships disabled and untrusted. Enable `localApi.enabled` and
`localApi.trustedPrivate` in that installation's `Config/settings.json`, then
restart Revia. Trusted private mode also enables the existing private-chat API;
keep its bearer token private.

Add the intended project directory to the approved roots through Revia's existing
permissions controls. In **Settings → Vision → App awareness**, enable
**Notice which app I'm using** (`perception.enabled`) and leave **Pause app
awareness for now** off. Turning awareness on requires a restart. Pausing or
revoking awareness stops new saved-file admission; screenshot permission is
separate and is not required by this route. Current companion, audience, source,
approved-root and file-read permissions are checked by the session on every event.

Choose the token file from the same running installation:
`RuntimeData/LocalApi/api-token`. Supply the file path explicitly; do not paste
the token into command arguments, source files or logs. The examples use the
default API port; substitute the configured loopback port when necessary.
On Windows this file contains DPAPI-protected bytes. The sender decrypts it only
for the same Windows user; do not replace it with a plaintext export. Invalid or
unreadable tokens are refused without rewriting the file.

Send the current saved revision once from the repository root:

```powershell
.\Tools\Presence\Send-EditorObservation.ps1 `
    -ProjectRoot 'C:\Work\Example' `
    -Path 'C:\Work\Example\Source\example.cpp' `
    -TokenFile 'C:\ReviaInstall\RuntimeData\LocalApi\api-token'
```

Or watch exactly that file until Ctrl+C:

```powershell
.\Tools\Presence\Watch-EditorFile.ps1 `
    -ProjectRoot 'C:\Work\Example' `
    -Path 'C:\Work\Example\Source\example.cpp' `
    -TokenFile 'C:\ReviaInstall\RuntimeData\LocalApi\api-token' `
    -DebounceMilliseconds 750
```

The watcher uses .NET `FileSystemWatcher`, does not recurse, and sends no initial
event. It waits for 500–1500 ms of quiet after a save and suppresses repeated
identical bytes. It unregisters its own events and disposes the watcher when the
script exits. Only a host-marked transient busy refusal gets up to three retries,
after one, two and four seconds; each attempt hashes the newest saved bytes. Only
202 marks a revision delivered. Policy refusals and uncertain requests are not
automatically retried; save a new revision or use the explicit sender after
resolving the reported refusal.

The scripts require valid saved UTF-8 and limit the file to 16 KiB. The session
accepts only its supported C/C++ extensions and may suppress stale, incomplete,
sensitive or changed files. A rejection is an admission result, not a compiler
diagnostic. No full-project understanding or build result is inferred from one file.
The completeness filter recognizes numeric digit separators and raw string
literals, including their prefixes and bounded delimiters. It remains a
nomination filter rather than a C++ parser or a compilation check.

An admitted source observation receives bounded native checks. The recognized
subset covers simple standard-vector searches with an inclusive size boundary
and returns of references to recognized automatic local objects. The diagnostic
distinguishes checked `at()` exceptions from unchecked index-precondition
violations, includes empty input and early successful returns, and describes
lifetime misuse conditionally. Function names, quoted expressions and minimal
corrections come from the current source. No watched source is compiled or run.

Unknown preprocessing, types, surrounding structure, additional control flow,
malformed or incomplete input, and oversized renderings are conservatively
declined. No supported finding means silence, not certification that the code is
safe. Native factual text is bounded and cannot be rewritten by model inference.
It still uses ordinary admission, publication, history, affect, speech and
cancellation owners. A relevant private factual follow-up recomputes from the
fresh typed saved-source context; unrelated, creative, structured and action
requests keep their normal routes. The legacy model-review API remains available
for its separate callers and evaluations, with its original accuracy limits.

The opt-in `ReviaCompanionAwarenessLive` executable accepts a fresh isolated
runtime directory, a new JSONL report path and one approved saved source file.
Its preflight requires a local owned model and disables memory, audio, cloud,
screenshots, auxiliary models and effectful tools. `--external-editor` waits for
the exact-file watcher to deliver a real saved revision. `--expect-silence`
requires an actual native silence decision and zero initiative comments
through shutdown; a timeout, suppressed reply or HTTP 202 cannot satisfy it.
The ordinary mode requires a comment and successful contextual follow-up.
Both modes retain actual output for separate semantic and personality review.
`--conversation-control` additionally submits ordinary story, poem and opinion
requests to the real model. Its route/transport checks do not replace semantic
review of those answers. Reports distinguish `native_cpp_subset` from a model
assessment and retain broad qualification flags as pending.
See [the continuation record](COMPANION_UPGRADE_PROGRESS.md) for current evidence
and outstanding acceptance work.

## Conversation and privacy controls

In **Settings → Chat**, choose **Normal**, **Talkative**, or **Focus**.
Normal keeps the configured interruption limits. Talkative shortens ordinary
cooldown within the same hourly maximum; it keeps dismissal, typing, call,
fullscreen and excluded-app gates. Focus suppresses optional openings while
ordinary replies and runtime warnings remain available. The hourly maximum `0`
means never. `/set initiative.mode focus` uses the same preference path.

An admitted source is a temporary snapshot, available for at most two minutes.
The existing background poll removes expired snapshots; pause, forget, awareness
off and session shutdown clear them immediately. Pending proposal records retain
metadata and a digest rather than raw source. Source text is not added as a user
turn or automatically classified into permanent memory. Only a delivered comment
joins the normal dialogue. A substantive private follow-up may use the snapshot
after its source and authorization are revalidated.

The native checks may find no supported diagnostic. A 202 receipt does not promise
an interruption or establish general code correctness. **Editor awareness** activity
appears in the existing runtime status feed; the ordinary app-awareness pause
control also pauses this integration. New direct input retires pending comments,
and a newer saved revision can cancel an older optional generation.

## HTTP contract

`POST http://127.0.0.1:17865/v1/editor/observations` requires the existing bearer
token and `application/json`. Browser Origins, foreign Host headers, chunked or
length-less bodies, and bodies over 8 KiB are refused before the body is read.
Only a trusted private API with its host-owned observation callback can admit it.

The JSON shape is closed, including rejection of duplicate keys:

```json
{
  "source": "saved_file",
  "projectRoot": "C:\\Work\\Example",
  "path": "C:\\Work\\Example\\Source\\example.cpp",
  "revision": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
  "occurredAtUnixMs": 1793206800000
}
```

The timestamp is an integer UTC epoch time in milliseconds. The sender supplies
the current time. The HTTP adapter bounds freshness; the session applies its
stricter current observation policy and rereads the approved file to verify the
digest. The client supplies neither source text nor instructions.

An admitted event returns **202** with `accepted`, `reason`, `eventId` and
`retryable: false`. This
acknowledges observation admission, not a review or spoken comment. A current
policy/source refusal returns **409** with `retryable: false`; a busy session may
return **409** with `retryable: true`. An unavailable or shared-mode callback
returns **403**, malformed input returns **400**, and callback failure returns
generic **503** without exception details. Invalid tokens return **401**.

The sender permits only the exact loopback HTTP route, disables proxies and
redirects, reads the token from the explicitly named file, and prints result
metadata without the token or source contents. The watcher has no permission
authority: a configured root never substitutes for Revia's approved roots or
current pause control.
