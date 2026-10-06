# Avatar bridge contract

`RuntimeData/Presence/avatar_state.json` is an atomic latest-state snapshot for an
isolated avatar renderer. `avatar_events.jsonl` is the ordered transition stream. Killing or
restarting the renderer does not affect Revia; it can reopen the snapshot and continue at
the newest sequence.

The version 1 snapshot contains:

- `phase`: `offline`, `idle`, `listening`, `thinking`, `responding`, `speaking`,
  `acting`, `waiting`, `blocked`, or `error`;
- `expression` and `affect_intensity`: the expression label and blend weight;
- `speaking`, `mouth`: the base lip-sync gate and value;
- `listening`: an animation and gaze cue;
- `attention` and `gaze_target`: a bounded label for the current target;
- `conversation_momentum`: a 0-1 idle-motion and engagement input;
- `sequence` and `timestamp`: restart-safe ordering;
- optional `mouth_track`: `started_at_ms`, `interval_ms` (50), and at most 2,400
  normalized 0…255 loudness values from Revia's own Qwen playback WAV.

A renderer should smooth `mouth`, expression, and gaze locally at its display frame rate.
The optional audio track follows the measured WAV loudness from successful
playback submission, so device latency remains estimated. It is independent of
general runtime phase and is cleared on completion, interruption and offline
state. SAPI/older producers retain the base gate. Phoneme-driven visemes remain
future work. Rendering remains a consumer: it must
never call inference or grant an action.

The canonical design, palette, expression mapping, and selected renderer target are in
`Config/avatar.json`. `target` remains `unselected` until a real Live2D/VRM model and its
renderer are chosen. The transition stream rotates to `avatar_events.jsonl.1` at the
configured byte ceiling, and repeated shutdown calls do not emit duplicate offline states.

The optional [Live2D adapter](Live2D/README.md) consumes this same snapshot and
injects custom tracking inputs into VTube Studio. `mouth` retains the legacy
binary gate; the adapter prefers a valid optional output-audio track. Snapshots
remain event-only rather than heartbeats. The bounded legacy timeout, track
limits and required model mappings are documented explicitly. Original
[reference art and rigging instructions](../../Assets/Avatar/Revia/README.md)
are accompanied by a local first rig and native export; further motion and
fresh corrected-build acceptance are recorded separately.
