# Revia in VTube Studio

A small Node program (Node 22 or newer, no npm packages) that gives Revia a Live2D face
through [VTube Studio](https://denchisoft.com/)'s public API. It reads the avatar state
Revia already writes (`RuntimeData/Presence/avatar_state.json`, see
[AVATAR_BRIDGE.md](../AVATAR_BRIDGE.md)) and drives whatever model VTube Studio has
loaded:

- **Mouth** while she speaks, from her speaking state: a syllable-rate envelope with
  pauses, which reads as talking. Visemes from her own voice timing are the planned
  replacement; the API has no audio lip-sync request, so audio never leaves Revia.
- **Expression** from her emotion, through a hotkey you map by name: the bridge writes
  `happy`, `angry`, `sulky` and so on, the config says which of your model's hotkeys
  each one triggers, and an expression with no mapping is left alone.
- **Gaze** toward whoever has her attention: the local user by default, the stream when
  chat has her, up and away while she thinks; with the blink and small idle look-around
  that keep a face from looking pinned.

It never calls inference and never grants an action. It is a consumer of the avatar
bridge, exactly as that contract intends.

## Set up

1. In VTube Studio, load your model and turn on the API (Settings, then the plug icon:
   *Start API*, port 8001).
2. Give the model hotkeys for the expressions you want her to show (Settings, Hotkey
   Settings). Their names are what the config maps.
3. Configure and check:

   ```powershell
   cd .\Tools\Presence\VTubeStudio
   Copy-Item config.example.json config.json
   notepad config.json
   node main.mjs config.json --check
   ```

   `statePath` is the **absolute** path of the running Revia's `avatar_state.json`;
   `tokenPath` is where the permission token is kept once granted (any absolute path
   under her RuntimeData is fine). Edit `expressionHotkeys` to your model's hotkey names.

4. Run it beside a running Revia:

   ```powershell
   node main.mjs config.json
   ```

   The first time, VTube Studio shows a permission popup; click **Allow**. The token is
   saved and reused after that.

## Test it without VTube Studio

```powershell
npm test
```

The tests run against a fake VTube Studio behind the WebSocket interface: the token
flow, the hotkey mapping, and the parameters re-sent each tick. The adapter has not yet
been run against a real VTube Studio; the API calls follow its published contract.

## What comes next

Visemes from Revia's own speech timing in place of the envelope; VRM behind the same
avatar bridge; and a desktop-pet window for assistant mode.
