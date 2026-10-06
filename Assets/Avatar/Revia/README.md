# Revia avatar artwork and Cubism handoff

`revia-front-v1.png` is original generated front-view reference art: 1024 × 1536,
RGBA with transparent surroundings. It follows the authored design in
[Config/avatar.json](../../../Config/avatar.json) and
[REVIA_CHARACTER_DESIGN.md](../../../docs/REVIA_CHARACTER_DESIGN.md).
The exact generation prompt is in `generation-prompt.txt`; generation used the
built-in image tool. This is a single flattened image, not a layered PSD or an
exported Live2D model. No third-party character or sample rig was substituted.

The editable 25-layer PSD and registered component PNGs are now in
[Live2D](Live2D/README.md). Its corrected head was imported and saved in Cubism
5.3.04 FREE. The current [compact-body rig](Live2D/Body-v2/README.md) is saved in
`Models/Live2D/Revia/Source/Revia-body-v2.cmo3` and exported locally. It has
initial expression, blink, speaking-mouth and lean controls. Blink uses basic
compression and may need visual polish; mouth control now accepts a WAV loudness
track with estimated playback timing and retains the older gate fallback.
Gaze, head X/Y turns, hair physics and independently animated limbs remain
unrigged. VTube Studio has loaded Revia and displayed the neutral pose.
Authentication and the eleven visual mappings are complete. Controlled live
checks confirmed the authored expression/mouth/lean controls and automatic
blink/breath. Actual conversation/speech worked with the original gate, and
renderer restart/reauthentication passed. The owner reported poor gate lip sync;
the corrected loudness-track build passed fresh real conversation acceptance on
2026-10-06. Two actual Qwen phrases drove varying mouth output and silent-window
closure, and Presence cleared the track after playback. Timing remains estimated.

## Layer preparation

Extend the current layered PSD for the full rig below. The current body is one
piece and each arm is one piece; independent clothing, finger and leg movement
still needs further separation. Draw hidden surfaces behind moving pieces:
cropped visible fragments alone leave holes during rotation. The expanded rig
should use separate, plainly named layers for:

| Group | Independently drawable parts |
| --- | --- |
| Head | Face base, ears, neck; neck behind face; no mouth baked into face |
| Eyes L / R | Sclera, iris, pupil, highlights, upper/lower lids, lashes and eyebrows |
| Mouth | Mouth cavity, upper/lower lips, teeth and tongue; clean closed-mouth face beneath |
| Hair | Back mass, front fringe, left/right side locks, cyan streak, clip and clip glow |
| Torso | Tunic, jacket back, left/right jacket fronts, collar, belt, signal core and glow |
| Arms L / R | Upper arm/sleeve, forearm/sleeve, cuff and hand |
| Lower body | Straight opaque trousers, each leg, each boot and boot accents |

Keep the navy/cyan/violet palette, covered practical outfit, compact proportions,
four-segment collar/clip motif and restrained technical detail. Preserve the face
and silhouette when repainting layer overlaps. Group glow overlays separately so
they can respond to attention and affect without recoloring the entire character.

## Cubism rig specification

The following is the intended full rig, beyond the initial controls above.
Parameter IDs in the export do not establish that every motion is authored.
Rig a stable closed-mouth neutral pose first. Establish face and body turn
deformers before secondary hair/jacket physics. Check all corners of X/Y/Z
combinations for holes, inverted meshes and eye/mouth drift.

| Cubism output parameter | Suggested range | Purpose / VTube Studio input |
| --- | --- | --- |
| ParamAngleX / Y / Z | -30…30 | Head turns; independent idle animation initially |
| ParamEyeLOpen / ParamEyeROpen | 0…1 | Blink; renderer-owned automatic blink |
| ParamEyeBallX | -1…1 | ReviaGazeX → gaze, with smoothing |
| ParamEyeBallY | -1…1 | Restrained renderer-owned idle gaze |
| ParamMouthOpenY | 0…1 | ReviaMouthGate 0…1 → output 0…0.65, temporary gate |
| ParamMouthForm | -1…1 | Optional joy/sadness expression combinations |
| ParamBodyAngleX | -10…10 | Restrained independent breathing/idle motion |
| ParamBreath | 0…1 | Renderer-owned breathing |
| ParamReviaJoy | 0…1 | ReviaJoy → brighter eyes, small smile, subtle magenta core |
| ParamReviaSadness | 0…1 | ReviaSadness → softer eyes, subdued violet core |
| ParamReviaAnger | 0…1 | ReviaAnger → brow tension, restrained amber/red core |
| ParamReviaFocus | 0…1 | ReviaFocus → steady gaze, brighter cyan streak/core |
| ParamReviaListening | 0…1 | ReviaListening → attentive small posture change |
| ParamReviaEngagement | 0…1 | ReviaEngagement → subtle idle-motion strength |

The proposed ParamRevia responses must be authored and checked in Cubism; the
first rig does not establish every response in this specification. The adapter
creates tracking inputs with names
beginning `Revia`, not these output parameters. VTube Studio mappings connect them.
Keep webcam mouth tracking off for the Revia mouth input. Qwen now supplies
measured loudness from Revia's own output WAV through that same input; older/SAPI
speech retains a gate. Detailed phoneme shapes require additional mouth rigging.

## Required production deliverables

The original layered `.psd` and the three-layer body update are checked in.
The current rig source is `Models/Live2D/Revia/Source/Revia-body-v2.cmo3`;
the earlier first rig and reference-matched source are retained.
The native export under `Models/Live2D/Revia/Runtime` contains
`Revia.model3.json`, `Revia.moc3`, `Revia.cdi3.json` and
`Revia.2048/texture_00.png`. Its parameter groups declare both eye-open parameters
for `EyeBlink` and `ParamMouthOpenY` for `LipSync`. No physics file is authored.

These runtime files were copied into the local VTube Studio model folder:

`D:/SteamLibrary/steamapps/common/VTube Studio/VTube Studio_Data/StreamingAssets/Live2DModels/Revia`

File installation and loading are complete, and the neutral pose was visually
checked in VTube Studio. Its Plugin API is enabled; authentication and mappings
are verified. The production adapter passed corrupt/missing/offline source
resets, recovery and the speaking-gate expiry against the actual renderer using
an isolated test snapshot. The running desktop now writes a fresh Presence
snapshot. Real speech and renderer restart are confirmed for the original gate
build. The corrected loudness-track build passed a fresh two-phrase runtime check
on 2026-10-06, including changing mouth output, silence and completion cleanup.
The compact body passed native export, neutral visual inspection, preserved-head
Core comparison and live control readback on the same date. The renderer in
`Config/avatar.json` is selected as `vtube_studio` with the local runtime model
path. Editable Cubism sources, runtime files and backups remain ignored local
model data; the body PSD, parts, previews and provenance are checked in.

Live2D documents the editor's
[runtime export procedure](https://docs.live2d.com/en/cubism-editor-manual/export-moc3-motion3-files/).
After loading, verify neutral pose, blink, mouth, authored affect/lean controls,
long speech, offline reset, disconnect and restart using the
[Revia VTube Studio adapter](../../../Tools/Presence/Live2D/README.md).
Finish and verify gaze, turns and physics before claiming those motions.
