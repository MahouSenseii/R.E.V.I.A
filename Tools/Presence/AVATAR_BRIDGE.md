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
