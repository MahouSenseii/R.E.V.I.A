# Revia on a VRM model, through VMC

A small Node program (Node 22 or newer, no npm packages) that gives Revia a 3D face
through the [VMC protocol](https://protocol.vmc.info/english): OSC over UDP to any
program that renders a VRM model and listens as a *Marionette* -- VSeeFace, VNyan,
Warudo, or your own. It reads the avatar state Revia already writes
(`RuntimeData/Presence/avatar_state.json`, see [AVATAR_BRIDGE.md](../AVATAR_BRIDGE.md))
and sends, thirty times a second:

- **Mouth** while she speaks, as the `A` (`aa`) viseme from a syllable-rate envelope with
  pauses. Visemes from her own voice timing are the planned replacement.
- **Expression** from her emotion, as blend-shape weights: `happy` becomes `Joy`, `sad`
  becomes `Sorrow`, and so on, scaled by her affect intensity and eased in and out so a
  change of mood is a change of face rather than a cut. `expressionBlendShapes` in the
  config maps each of her expressions to the shapes and weights you want; an
  expression with no mapping is left alone.
- **Gaze** as the `Head` bone turned toward whoever has her attention: the local user
  by default, the stream when chat has her, up and away while she thinks; with the
  blink and small idle look-around that keep a face from looking pinned.

It never calls inference and never grants an action. It is a consumer of the avatar
bridge, exactly as that contract intends, beside the [VTube Studio adapter](../VTubeStudio/README.md)
for Live2D.

## Set up

1. In your renderer, load the VRM and turn on VMC receiving (VSeeFace: *General
   settings*, *OSC/VMC receiver*, port 39539; VNyan and Warudo have a *VMC receiver*
   node or plugin with the same default). Leave its own tracking off for the face and
   head, or the two will fight.
2. Pick the blend-shape names your model has: `vrm0` (`A`, `Joy`, `Blink_L`, ...) or
   `vrm1` (`aa`, `happy`, `blinkLeft`, ...).
3. Configure and check:

   ```powershell
   cd .\Tools\Presence\VMC
   Copy-Item config.example.json config.json
   notepad config.json
   node main.mjs config.json --check
   ```

   `statePath` is the **absolute** path of the running Revia's `avatar_state.json`.

4. Run it beside a running Revia:

   ```powershell
   node main.mjs config.json
   ```

Each tick is one OSC bundle (`/VMC/Ext/OK`, `/VMC/Ext/T`, `/VMC/Ext/Bone/Pos Head`,
`/VMC/Ext/Blend/Val` per shape, `/VMC/Ext/Blend/Apply`). A receiver that does not take
bundles gets one datagram per message with `"bundle": false`.

## Test it without a renderer

```powershell
npm test
```

The tests decode the OSC with a reader written apart from the encoder, receive the
bundles on a real UDP socket, and check the mouth, the blink, the easing of an
expression, the head turned toward the stream and the release of a shape no longer
wanted. The adapter has not yet been run against a real VMC receiver; the messages
follow the published protocol.

## What comes next

Visemes from Revia's own speech timing in place of the envelope; body idle motion
through more bones; and the same face in the Revia-Stage VM when it exists.
