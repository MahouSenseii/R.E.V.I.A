# Revia Live2D presentation adapter

This optional, dependency-free Node.js adapter reads Revia's existing Presence
snapshot and sends eight owned tracking inputs to a local VTube Studio instance.
It never sends conversation text, reads webcam frames, calls inference, changes
memory or submits actions. Closing the renderer leaves Revia running.

## Current status

Original reference artwork is saved in
[Assets/Avatar/Revia](../../../Assets/Avatar/Revia/README.md).
The adapter has automated contract checks. A 25-layer PSD and a locally saved,
reference-matched Cubism source project are available. The animation rig, runtime
export, VTube Studio import and live acceptance test are still required. The
checked-in renderer target remains `unselected`; the adapter does not change it.

## Setup

1. Install [VTube Studio](https://denchisoft.com/) and prepare the model using
   [Live2D Cubism](https://www.live2d.com/en/cubism/). Complete the
   [layer and rig handoff](../../../Assets/Avatar/Revia/README.md). Import the
   exported Revia model into VTube Studio and select it manually.
2. Use Node.js 22.12 or newer. No npm install or API key is required. In VTube
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
   | ReviaGazeX | ParamEyeBallX | -1…1 |
   | ReviaEngagement | ParamReviaEngagement | 0…1 |

   Custom output parameters require the actual authored rig. Creation of an input
   alone does not create the corresponding visual deformation. Add restrained
   smoothing in VTube Studio, automatic blinking, breathing and hair physics.

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

Presence currently exports a binary speaking gate, **not output-audio amplitude**.
The gate is useful to wire the pipeline but does not produce natural syllable lip
sync. A gate older than 15 seconds closes the mouth and stops transient listening
cues. Because snapshots are written on transitions rather than as heartbeats,
this can close the mouth during a healthy long utterance; it does not prove a
crash. Retain valid mood/focus/engagement until a new snapshot arrives. Missing,
invalid, unsupported or explicit offline state resets all inputs. A future
timestamp more than five seconds ahead is refused.

Orderly shutdown sends neutral values before closing. An unexpected adapter
termination relies on VTube Studio's documented one-second parameter expiry.
Transport failures during active injection reconnect with the saved token.
Startup/initialization failures stop with a useful error; denied authentication
does not trigger an approval loop. No public or remote control server is opened.

## Verification

```powershell
node --test Tests/live2dBridge.test.mjs
```

The automated suite exercises snapshot admission, bounds, mood aliases, freshness,
endpoint restriction, request correlation, authentication, timeout/disconnect and
neutral shutdown using a simulated renderer. It does not prove that the absent
Revia rig renders. Before calling the avatar live, verify the real imported model
against all mappings, long speech, source removal, offline state and VTube Studio
restart. Then record the actual model path and select `vtube_studio` in the authored
renderer configuration.
