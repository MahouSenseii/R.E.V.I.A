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

`Face_Underpaint` is generated skin backing for later overlap work. It is hidden
in neutral, as are the retained legacy mouth cavity and lower-lip sources. Their
alignment, color and coverage must be finished with the expression keyforms.
Do not simply enable those layers and call the result an authored expression.

The PSD was imported into Live2D Cubism Editor 5.3.04 FREE and saved locally as:

`Models/Live2D/Revia/Source/Revia-reference-matched.cmo3`

The previous `.cmo3` remains intact. The previous PSD, preview and manifest are
preserved in `Models/Live2D/Revia/Source/Before-reference-head`. These are ignored
local model data.

## Production state

This is an editable neutral source with corrected static likeness. It has no
accepted animation rig, texture atlas, exported MOC3 or verified VTube Studio
parameter mappings. Default Cubism sliders do not demonstrate authored movement.
Continue with the rig specification in the [avatar handoff](../README.md).

Visible cutouts need overlap painting and edge refinement before deformation.
The skin backing is a starting asset, not proof that every hidden surface is
finished. The iris layers still combine pupils and highlights; the body and each
arm remain single pieces. Clothing, fingers and legs need further separation for
independent motion. Verify blinks, mouth shapes, gaze and head-turn extremes.

## Provenance and checks

The original generated head/body atlases and their prompts remain in `Source`
for provenance. The current visible head uses the approved reference instead of
the old head atlas. `underpaint-prompt.txt` records the built-in image tool edit
used only for the concealed skin backing; it does not replace the visible face.

Technical packaging uses Sharp 0.35.4 and ag-psd 31.0.2. Verification reads the
PSD back and checks every layer's pixels, placement and visibility against its
PNG and manifest. The unoccluded upper head was compared to the uniformly scaled
reference: 22,614 near-opaque pixels differed by at most one 8-bit channel level
after compositing. This checks static image fidelity, not animation quality.
