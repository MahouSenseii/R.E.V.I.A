# Revia compact body v2

The current body replaces the earlier adult silhouette with an approximately
5.5-head figure: a straight torso, loose high-collar tunic and technical jacket,
opaque straight trousers, shorter limbs and compact boots. Revia's approved
face, asymmetric hair, cyan streak, violet accents and personality are retained.

Hiyori supplies a proportion guide only. The installed neutral Hiyori sample
measured approximately 5.49 heads; this package's registered nominal landmarks
give 5.480 heads, compared with approximately 6.69 for the previous Revia body.
This is original Revia artwork, not Hiyori artwork or a substituted sample rig.
The editable Hiyori_A project was not available locally; measurements use the
installed VTube Studio Hiyori sample and should not be treated as exact Hiyori_A
geometry. See the official [Hiyori sample](https://www.live2d.com/en/learn/sample/momose-hiyori/).

## Artwork and source package

- `body-update.psd`: 1024 × 1536, containing only `Body`, `Arm_ViewerLeft` and
  `Arm_ViewerRight`. It is a replacement source, not a complete rig PSD.
- `Parts/`: the matching transparent drawable PNGs.
- `fullbody-preview.png` and `fullbody-preview-gray.png`: the new body composed
  with the original approved head; the gray version makes edges readable.
- `body-only-preview.png`: the three replacement parts together.
- `Source/new-body-reference.png`: the selected generated source, retained verbatim.
- `Source/generation-prompt.txt`: both exact image-edit prompts and input/output
  provenance. Generated head pixels are excluded from the replacement parts.
- `Source/manifest.json`: source hashes, masks, transform, placement and package proof.
- `Source/package-body.cjs`: reproducible technical registration and PSD assembly.

The original 25-layer PSD, layout and all 22 original head/neck PNGs remain
unchanged. The replacement body is uniformly scaled by 0.685, with its source
collar `(522, 300)` registered to model collar `(512, 432)`. The neckline mask
retains the jacket lining underneath the approved hair. Seams partition admitted
pixels without neutral overlaps or missing pixels; they do not author hidden
surfaces for independent future limb rotations.

## Native rig and installation

The saved local Cubism source is
`Models/Live2D/Revia/Source/Revia-body-v2.cmo3`. The earlier first rig is intact.
The update PSD was registered without adding ArtMeshes, then its three images
were assigned to the existing same-named meshes. Those three unkeyed meshes were
regenerated with Cubism's Standard preset, retaining their IDs and
`ReviaRootMotion` parent. The existing atlas refreshed at 100% scale; layout
inspection selected no overlap or out-of-frame textures.

The native SDK 5.0 export is `Models/Live2D/Revia/Runtime/Revia.model3.json`, with
`Revia.moc3`, `Revia.cdi3.json` and `Revia.2048/texture_00.png`. It is installed in:

`D:/SteamLibrary/steamapps/common/VTube Studio/VTube Studio_Data/StreamingAssets/Live2DModels/Revia`

The existing VTube Studio model identity, approved plugin connection and eleven
mappings were retained. The installed model was reloaded and visually checked
with the new body. `Config/avatar.json` now selects `vtube_studio` and the local
runtime model path. This metadata does not auto-start the adapter.

The previous installed model and settings are backed up under
`Models/Live2D/Revia/Before-body-v2-20261006`. Native sources, runtime exports,
backups and authentication remain ignored local model data. A fresh clone needs
the native source/export copied separately or reimported and rigged in Cubism;
the checked-in PSD alone does not provide the authored keyforms.

## Verification on 2026-10-06

Technical package verification read the PSD back and confirmed all three layer
pixels and placements against the PNGs, exact body reconstruction, and unchanged
original head pixels and input hashes. The recorded tool versions are Sharp
0.35.4 and ag-psd 31.0.2, pinned in `Source/package.json`.

Independent inspection through the installed Cubism Core confirmed the same
24 mesh IDs, all 30 parameter definitions and the same 1024 × 1536 canvas. All
21 retained head/neck meshes have exactly zero neutral coordinate difference,
with matching counts, parent parts and opacity. Only Body and the two arms
changed geometry. The export and installed MOC3 share SHA-256
`8e3a72c39667e000421309423297e976cf6741cf3a8c03573e3ad522ae996b16`.

Live VTube Studio readback confirmed mouth output 0…0.65, joy and sadness at 0.8,
anger at 0.75, focus at 0.6, listening at 1 and engagement at 0.8. These were
controlled visual probes, not Revia conversations. A separate idle sample saw
automatic eye-open values 0.0405…1 and breath 0.000026…0.999909; the mouth stayed
closed. The production adapter was then restored to the running Revia snapshot.

The corrected Qwen build also passed an actual two-phrase conversation earlier
on this date: output loudness changed the renderer mouth, silent windows closed
it, and completion cleared the track. Device timing remains estimated from
playback submission. The body replacement preserves the existing mouth rig.
A subsequent real response with the new body loaded published 156 loudness
windows at 50 ms, including 37 silent windows, and cleared the track after
completion. Its Presence events confirm source production and cleanup; the
post-replacement live probes above separately verify renderer controls.

Blink is still basic compression and the mouth uses a simple opening shape.
Head X/Y turns, animated gaze, hair physics and independent fingers, clothing or
legs remain further rigging work. This body update does not claim those motions.

## Rebuild the technical package

Install the pinned dependencies beside the script, then run it from any directory:

```powershell
npm install --prefix Assets/Avatar/Revia/Live2D/Body-v2/Source --no-audit --no-fund
node Assets/Avatar/Revia/Live2D/Body-v2/Source/package-body.cjs
```

Alternatively set `REVIA_SHARP_MODULE` and `REVIA_AG_PSD_MODULE` to existing
package directories. The script writes only this Body-v2 package, preserves the
original sources and asserts its pixel/registration proof before finishing.
