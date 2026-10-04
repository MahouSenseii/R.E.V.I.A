# Revia avatar artwork and Cubism handoff

`revia-front-v1.png` is original generated front-view reference art: 1024 × 1536,
RGBA with transparent surroundings. It follows the authored design in
[Config/avatar.json](../../../Config/avatar.json) and
[REVIA_CHARACTER_DESIGN.md](../../../docs/REVIA_CHARACTER_DESIGN.md).
The exact generation prompt is in `generation-prompt.txt`; generation used the
built-in image tool. This is a single flattened image, not a layered PSD or an
exported Live2D model. No third-party character or sample rig was substituted.

The editable 23-layer PSD and registered component PNGs are now in
[Live2D](Live2D/README.md). Its corrected head was imported and saved in Cubism
5.3.04 FREE. The animation rig and VTube Studio runtime verification remain open.

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
| Lower body | Shorts, each leg/tights, each boot and boot accents |

Keep the navy/cyan/violet palette, covered practical outfit, adult proportions,
four-segment collar/clip motif and restrained technical detail. Preserve the face
and silhouette when repainting layer overlaps. Group glow overlays separately so
they can respond to attention and affect without recoloring the entire character.

## Cubism rig specification

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

The custom ParamRevia outputs are a rigging specification; they must actually be
created and keyed in Cubism. The adapter creates tracking inputs with names
beginning `Revia`, not these output parameters. VTube Studio mappings connect them.
Do not map webcam mouth tracking on top of the Revia mouth gate. For polished lip
sync, replace that mapping with measured Revia output audio, not the user's mic.

## Required production deliverables

The layered `.psd` is checked in. A corrected, unrigged `.cmo3` was saved locally
under `Models/Live2D/Revia/Source/Revia-head-corrected.cmo3`. No `.moc3` has been
exported. Complete the rig, then export a real `Revia.model3.json`, its referenced
`.moc3` and texture atlas files, plus configured `.physics3.json` and any authored
expressions/motions.
Keep editable source files with the artist/rigging project and deploy runtime files
as local user data under `Models/Live2D/Revia` or VTube Studio's model folder.

Live2D documents the editor's
[runtime export procedure](https://docs.live2d.com/en/cubism-editor-manual/export-moc3-motion3-files/).
After import, verify neutral pose, blink, gaze, every affect input, long speech,
offline reset, disconnect and restart using the
[Revia VTube Studio adapter](../../../Tools/Presence/Live2D/README.md).
Only then select the real renderer/model in `Config/avatar.json`.
