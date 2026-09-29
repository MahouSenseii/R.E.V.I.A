# Avatar bridge contract

`RuntimeData/Presence/avatar_state.json` is an atomic latest-state snapshot for an
isolated VRM renderer. `avatar_events.jsonl` is the ordered transition stream. Killing or
restarting the renderer does not affect Revia; it can reopen the snapshot and continue at
the newest sequence.

The version 1 snapshot contains:

- `phase`: `offline`, `idle`, `listening`, `thinking`, `responding`, `speaking`,
  `acting`, `waiting`, `blocked`, or `error`;
- `expression` and `affect_intensity`: the VRM expression preset and blend weight;
- `speaking`, `mouth`: the base lip-sync gate and value;
- `listening`: an animation and gaze cue;
- `attention` and `gaze_target`: a bounded label for the current target;
- `conversation_momentum`: a 0-1 idle-motion and engagement input;
- `sequence` and `timestamp`: restart-safe ordering.

A renderer should smooth `mouth`, expression, and gaze locally at its display frame rate.
Audio-amplitude or phoneme-driven visemes can replace the base mouth gate later without
changing the state owner or the rest of the schema. Rendering remains a consumer: it must
never call inference or grant an action.

The first consumer is the [VTube Studio adapter](VTubeStudio/README.md), which drives a
Live2D model from this snapshot alone: mouth from `speaking`, a hotkey from `expression`,
gaze from `attention`. The second is the [VMC adapter](VMC/README.md), which drives a VRM
model through any VMC receiver from the same snapshot: a mouth viseme, blend-shape
weights from `expression` and `affect_intensity`, the head bone from `attention`.

The canonical design, palette, expression mapping, and selected renderer target are in
`Config/avatar.json`. `target` remains `unselected` until a real Live2D/VRM model and its
renderer are chosen. The transition stream rotates to `avatar_events.jsonl.1` at the
configured byte ceiling, and repeated shutdown calls do not emit duplicate offline states.
