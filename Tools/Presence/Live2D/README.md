# Revia Live2D presentation adapter

This optional Node.js adapter reads Revia's existing Presence
snapshot and sends eight owned tracking inputs to a local VTube Studio instance.
It never sends conversation text, reads webcam frames, calls inference, changes
memory or submits actions. Closing the renderer leaves Revia running.

## Current status

Original reference artwork is saved in
[Assets/Avatar/Revia](../../../Assets/Avatar/Revia/README.md).
The adapter has automated contract checks. A 25-layer PSD and a locally saved,
reference-matched first rig are available. The rig source is
`Models/Live2D/Revia/Source/Revia-first-rig.cmo3`; its native export is
`Models/Live2D/Revia/Runtime/Revia.model3.json`. Initial expression, blink,
speaking-mouth and lean controls are authored; gaze, head X/Y turns, hair physics
and independently animated limbs remain unrigged. Blink uses basic compression
and may need visual polish. Qwen speech now supplies a loudness track derived
from its own WAV; older/SAPI snapshots retain the binary speaking gate.

The export files are installed in the local VTube Studio model folder recorded
in the [handoff](../../../Assets/Avatar/Revia/README.md). VTube Studio has loaded
and visually displayed Revia's neutral pose. Revia Presence authenticated with
the enabled Plugin API, and all eleven visual mappings were applied with a
backup of the model settings. Controlled live checks confirmed mouth, joy,
sadness, anger, focus, listening and engagement controls, plus automatic blink
and breath. The production adapter also passed real-renderer checks for corrupt,
missing and offline source resets, source recovery and the 15-second mouth-gate
expiry. These used a separate test snapshot. Revia's current desktop instance
now supplies a fresh real Presence snapshot. Real replies and speech ran, and
the owner confirmed the avatar worked but its original gate lip sync was off.
VTube Studio was restarted: the adapter reconnected with its saved approval,
and Revia's model/mappings remained loaded. The corrected loudness-track build
still needs a fresh real conversation check.
The checked-in renderer target remains
`unselected`; the adapter does not change it.

## Setup

1. Install [VTube Studio](https://denchisoft.com/) and prepare the model using
   [Live2D Cubism](https://www.live2d.com/en/cubism/). Complete the
   [layer and rig handoff](../../../Assets/Avatar/Revia/README.md). Import the
   exported Revia model into VTube Studio and select it manually.
2. Use Node.js 22.12 or newer. Install the pinned WebSocket transport once from
   the repository root:

   ```powershell
   npm ci --prefix Tools/Presence/Live2D --ignore-scripts --no-audit --no-fund
   ```

   No inference API key is required. In VTube
   Studio settings enable **Allow Plugin API access**, using port 8001 unless
   you explicitly select another local port.
3. From the repository root, inspect the actual running companion's state file:

   ```powershell
   node Tools/Presence/Live2D/main.mjs --state "C:\path\to\running\Revia\RuntimeData\Presence\avatar_state.json" --inspect
   ```

   Pass the captured companion path for a non-legacy companion. Copy the exact
   **Avatar state** path shown in Revia's Presence panel. Use the state file
   belonging to the running companion, not an old snapshot in another build.
   The example path is a placeholder; `--inspect` does not connect or save credentials.
   When PowerShell permits local scripts, the Windows launcher accepts the same
   explicit source from another directory when invoked by its full path:

   ```powershell
   & "C:\path\to\R.E.V.I.A\Tools\Presence\Live2D\StartLive2D.ps1" -StatePath "C:\path\to\running\Revia\RuntimeData\Presence\avatar_state.json" -Inspect
   ```

   Omit `-Inspect` to connect. `-Endpoint` and `-AuthFile` forward the corresponding
   adapter options; Node.js must be on `PATH`. The launcher requires `-StatePath`
   and does not select a companion, start Revia/VTube Studio or choose a model.
   If PowerShell's script policy blocks the launcher, use Node with the same
   absolute paths:

   ```powershell
   node "C:\path\to\R.E.V.I.A\Tools\Presence\Live2D\main.mjs" --state "C:\path\to\running\Revia\RuntimeData\Presence\avatar_state.json" --inspect
   ```
4. Start the adapter with the same path, omitting `--inspect`. Approve
   **Revia Presence** by **Revia Project** in VTube Studio's native plugin dialog.
   This approval is required by VTube Studio's API. A token is saved only after
   successful authentication, under ignored
   `RuntimeData/Presence/Live2D/vts_auth.json`. It is a private local credential;
   do not commit it. Windows uses the containing user's directory permissions.
   Reconnects reuse the same approval; denial/revocation stops the adapter.
5. Configure the imported model's input/output mappings:

   | Input | Output | Range |
   | --- | --- | --- |
   | ReviaMouthGate | ParamMouthOpenY | input 0…1 → output 0…0.65 |
   | ReviaJoy | ParamReviaJoy | 0…1 |
   | ReviaSadness | ParamReviaSadness | 0…1 |
   | ReviaAnger | ParamReviaAnger | 0…1 |
   | ReviaFocus | ParamReviaFocus | 0…1 |
   | ReviaListening | ParamReviaListening | 0…1 |
   | ReviaGazeX | ParamEyeBallX | -1…1; gaze motion not yet rigged |
   | ReviaEngagement | ParamReviaEngagement | 0…1 |

   Custom output parameters require the actual authored rig. Creation of an input
   alone does not create the corresponding visual deformation. Use restrained
   smoothing and verify automatic blink against the basic compression keyforms.
   The export declares `EyeBlink` and `LipSync` groups; those declarations do not
   verify renderer behavior. Gaze and hair physics need further rigging.

   [Revia-first-rig-mappings.json](../../../Assets/Avatar/Revia/Live2D/Revia-first-rig-mappings.json)
   records the first rig's visual mappings, including automatic blink and breath.
   It uses VTube Studio's `ParameterSettings` row format. Replace existing rows
   with the same `OutputLive2D`; appending would leave competing writers for the
   mouth, eyes and breath. Keep unrelated model settings and a backup. Unload the
   model before editing its saved visual mappings, then reload and verify them.

The official
[VTube Studio API contract](https://github.com/DenchiSoft/VTubeStudio)
documents authentication, custom parameter creation and injection. The adapter
uses tracking inputs rather than attempting to write arbitrary Cubism parameters.
`faceFound` is a renderer activity cue; it is not evidence of seeing a real face.

## Behavior and limits

The default source path is `RuntimeData/Presence/avatar_state.json` under the
repository root. Override it for installed builds/private companions. The renderer
endpoint accepts only `ws://127.0.0.1`, `ws://localhost` or `ws://[::1]` with a port;
`--endpoint ws://127.0.0.1:8002` selects another instance. `--auth-file PATH`
selects a private credential location; never use a checked-in directory.

Input injection runs at at most 20 Hz with one request in flight. This is
presentation scheduling, not a measured animation frame rate. VTube Studio handles
rendering/smoothing. The adapter does not auto-load or replace a model, and cannot
verify mappings merely because a model is loaded.

Qwen playback publishes a bounded `mouth_track`: its own WAV's normalized RMS
in 50 ms windows, indexed from successful playback submission. The track closes
the mouth during silent windows and at its end, and continues independently of
next-phrase generation status. It is measured loudness with **estimated playback
timing**, not an audio-device cursor or phoneme/viseme lip sync. Audio-device
latency can still shift the match. Tracks cover at most 120 seconds; the adapter
limits a snapshot to 64 KiB and validates the sample/time bounds.

Older builds and SAPI without a track keep the binary speaking gate. A gate
older than 15 seconds closes the mouth and stops transient listening cues.
Because those snapshots are event-only, this can close the mouth during a
healthy long utterance; it does not prove a crash. A valid audio track has its
own end time and does not use that 15-second cutoff. Retain valid mood/focus/
engagement until a new snapshot arrives. Missing,
invalid, unsupported or explicit offline state resets all inputs. A future
timestamp more than five seconds ahead is refused.

Orderly shutdown sends neutral values before closing. An unexpected adapter
termination relies on VTube Studio's documented one-second parameter expiry.
After a renderer session is established, connection failures, send failures and
request timeouts reconnect with the saved token, including a disconnect between
polls. Each failed recovery attempt waits two seconds before retrying connection
and initialization while VTube Studio is unavailable. Cancellation stops those
retries. Initial connection or initialization failures stop with a useful error.
Authentication denial/revocation and API errors stop without requesting approval
again. No public or remote control server is opened.

## Verification

```powershell
node --test Tests/live2dBridge.test.mjs
```

The automated suite exercises snapshot admission, bounds, mood aliases, freshness,
endpoint restriction, request correlation, authentication, timeout/disconnect and
neutral shutdown using a simulated renderer. It does not prove that the real
Revia model renders. Before calling the avatar live, load the installed model and
verify its neutral pose, blink, mouth and authored expression/lean mappings,
long speech, source removal, offline state and VTube Studio restart. Keep
unrigged gaze and physics outside that acceptance claim. Then record the actual
model path and select `vtube_studio` in the authored renderer configuration.

The opt-in `Tests/live2dRenderer.live.test.mjs` check uses only repeated
`APIStateRequest` calls against the running local VTube Studio API. It neither
requests plugin approval nor changes a model. This catches the observed case
where Node's built-in WebSocket received an empty string for every response after
the first. The adapter uses the pinned `ws` transport with compression disabled;
simulated renderer checks alone did not detect that incompatibility. To include
the live status check while VTube Studio's API is enabled:

```powershell
$env:REVIA_LIVE_VTS = '1'
node --test Tests/live2dBridge.test.mjs Tests/live2dRenderer.live.test.mjs
Remove-Item Env:REVIA_LIVE_VTS
```

The three status replies passed on the development PC after the transport fix.
The 24 simulated checks also passed. Real model controls and source reset checks
above are separate from this read-only transport regression.
