# Saved-file editor observations

The first editor integration reports one explicitly selected saved C/C++ file.
It does not read unsaved editor buffers, follow the cursor, take screenshots,
execute commands, generate a reply through the HTTP endpoint, or establish that
code builds or passes tests. Authorship is unknown. The existing session decides
whether an admitted observation merits a later, bounded comment.

## Validation status and moving to another computer

This Stage A integration is partially verified. Native/script regressions cover
admission, modes, cancellation, silence and private follow-up continuity. Earlier
real-model trials connected the saved-file watcher to a comment and follow-up, but
the code assessment was incorrect. The latest reasoning-budget and continuity
repairs still need fresh real-model qualification. Passing tests or receiving HTTP
202 does not establish useful code review.

The last live preflight used an RTX 3070 Laptop GPU with 8 GiB VRAM and 16 GiB RAM.
Windows reported other microphone use, and available RAM was about 1.4 GiB. The
owner chose to record that test limit. No attention bypass or final model trial
was performed. RTX 5070 behavior, physical voice, image/video generation and
generation-time conversation performance are not qualified by this stage.

On another computer, pull main and build with `Tools/Build.ps1`. Verify that
machine's actual model paths, hardware headroom and permissions before enabling
awareness. Models, personal memories, runtime settings and credentials are local
data and are not transferred by a Git pull. Use a private transfer for personal
data. A Windows DPAPI token belongs to its original user/machine; let the existing
local API owner provision a token on the destination instead of copying or
exporting the old token as plaintext.

The next live check is a meaningful saved edit, its unsolicited comment and a
contextual direct follow-up, followed by a safe-code scenario where silence is
appropriate. Run with an idle microphone and measured memory headroom. Inspect
the actual model output before accepting Stage A; retain unsuccessful trials.

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

The model may choose silence. A 202 receipt does not promise an interruption or
establish that its code assessment is correct. **Editor awareness** activity
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
