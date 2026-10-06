# Revia layered artwork

`revia-parts.psd` contains 25 independent RGBA layers on a 1024 x 1536 canvas.
`Parts` contains the individual drawable PNGs. `Source/layout.json` records their
positions, neutral visibility, reference transform and head segmentation polygons.
`revia-layered-preview.png` is the composite of the visible layers.

## Reference-matched head

Version 3 replaces the generated head interpretation with visible pixels from the
owner's approved reference, retained in `Source/revia-approved-reference.png`.
The earlier atlas prompt requested a symmetric front view and the assembly
stretched individual pieces. Scaling that assembly did not restore the likeness.

The reference head now keeps its compact jaw, tilted eyes, iris colors, subtle
nose and smile, asymmetrical bangs, loose crown strand and hanging earring. Its
uniform scale is 0.8; reference collar point `(512, 250)` maps to model collar
point `(512, 432)`, with integer raster rounding recorded in the manifest.
The body and arms keep their existing artwork and registration.

The visible head is partitioned into independent face, neck, eye whites, irises,
upper/lower lids, brows, smile, hair sections, clip and earring layers. Each
reference pixel belongs to one visible layer. This preserves neutral likeness
without baking the complete head into a single replacement layer.

The original PSD retains its hidden skin backing and legacy mouth sources.
The first native rig uses the reference-aligned
[backing v2](Revia-rig-backing-v2/README.md) and natural, fang-free
[speaking mouth v1](Revia-rig-mouth-v1/README.md). Both replacements were
registered, remeshed and keyed in the native rig; the original PSD and part PNGs
remain unchanged. Their package proofs cover source pixels and placement.
Controlled VTube Studio playback confirmed the mouth and expression controls;
natural speech and further deformation still need acceptance.

The PSD was imported into Live2D Cubism Editor 5.3.04 FREE and saved locally as:

`Models/Live2D/Revia/Source/Revia-reference-matched.cmo3`

The previous `.cmo3` remains intact. The previous PSD, preview and manifest are
preserved in `Models/Live2D/Revia/Source/Before-reference-head`. These are ignored
local model data.

## Production state

The first rig is saved as `Models/Live2D/Revia/Source/Revia-first-rig.cmo3`.
Its local export under `Models/Live2D/Revia/Runtime` contains the model descriptor,
MOC3, display metadata and a 2048 x 2048 texture atlas, with populated `EyeBlink`
and `LipSync` groups. Initial expression, blink, speaking-mouth and lean controls
are authored. Blink is basic compression and may need visual polish; mouth
control accepts Revia's output loudness track, with a binary gate fallback for
older/SAPI speech. The simple mouth artwork remains a first-pass shape.

Runtime files have been installed in the VTube Studio model folder recorded in
the [avatar handoff](../README.md). Revia has been loaded and its neutral pose
visually checked in VTube Studio. Revia Presence authentication succeeded and the
eleven preset mappings are installed. Controlled live playback and parameter
readback confirmed the authored expressions, mouth and lean, plus automatic
blink and breath. The production adapter passed source reset/recovery and
transient-gate expiry checks against the real renderer using an isolated test
snapshot. Real conversation/speech worked with the original gate, but the owner
reported poor lip sync. Renderer restart/reauthentication passed. The corrected
loudness-track build still needs a fresh actual conversation check. Gaze,
head X/Y turns, hair physics and independent limb movement remain unrigged. The
handoff's full rig specification remains a production target. The
[first-rig mapping preset](Revia-first-rig-mappings.json) records the eight bridge
inputs and automatic blink/breath mappings without changing app permissions.

Further deformation needs overlap and edge checks; the backing does not prove
every hidden surface is finished. The iris layers still combine pupils and
highlights; the body and each arm remain single pieces. Clothing, fingers and
legs need further separation for
independent motion. Verify the current blink/mouth forms and any future gaze or
head-turn extremes in the renderer.

## Provenance and checks

The original generated head/body atlases and their prompts remain in `Source`
for provenance. The current visible head uses the approved reference instead of
the old head atlas. `underpaint-prompt.txt` records the original skin edit. The
backing-v2 and mouth-v1 directories preserve exact prompts and packaging proofs.
Their selected edits remain in `Source/revia-reference-underpaint-v2.png` and
`Revia-rig-mouth-v1/generated-reference.png` respectively.

Technical packaging uses Sharp 0.35.4 and ag-psd 31.0.2. Verification reads the
PSD back and checks every layer's pixels, placement and visibility against its
PNG and manifest. The unoccluded upper head was compared to the uniformly scaled
reference: 22,614 near-opaque pixels differed by at most one 8-bit channel level
after compositing. This checks static image fidelity, not animation quality.
