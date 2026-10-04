# Revia layered artwork

`revia-parts.psd` contains 23 independent RGBA layers on a 1024 x 1536 canvas.
`Parts` contains the individual drawable PNGs. `Source/layout.json` records their
source rectangles, crop bounds, final canvas positions and neutral visibility.
`revia-layered-preview.png` is the composite of the visible layers.

## Head correction

The first assembly made the head too large and left too much neck exposed. Version
2 applies a uniform 0.78 scale to every head component around canvas point
`(512, 432)`, then translates the group down 20 pixels. Face, eyes, eyebrows,
mouth, hair, neck and clip keep a shared coordinate transform. The body and arms
retain their registration.

The sclera artwork already includes lower contours, and the closed-mouth artwork
already includes lip shading. Separate lower-lid and lower-lip layers are retained
but hidden in the neutral pose to avoid doubled lines. The mouth cavity is also
hidden until a real open-mouth form is authored.

The corrected PSD was imported as a fresh model in Live2D Cubism Editor 5.3.04
FREE, visually checked at the full-canvas view, and saved locally as:

`Models/Live2D/Revia/Source/Revia-head-corrected.cmo3`

The earlier editor state and PSD were preserved beside that project with
`before-head-fix` names. These working projects are ignored local model data.

## Production state

This is an editable neutral model source. It does not yet have an accepted
animation rig, final texture atlas, exported MOC3 or verified VTube Studio
parameter mappings. Default parameter sliders created by Cubism are not evidence
of authored movement. Continue with the rig specification in the
[avatar handoff](../README.md).

The 23 layers include separate facial features and hair, a central body, and two
arms. Teeth and tongue remain combined with the mouth cavity; pupils and eye
highlights remain combined with each iris. Clothing, fingers and legs need
additional separation for independent motion. Inspect alpha edges and hidden
surfaces during mesh and deformation work.

## Provenance and checks

The two source atlases were generated with the built-in image tool using the
approved `revia-front-v1.png` as the reference. Exact prompts are retained in
`Source/head-prompt.txt` and `Source/body-prompt.txt`. The correction adjusts native
layer registration and visibility; it does not generate replacement character art.

Technical packaging used Sharp 0.35.4 and ag-psd 31.0.2. The alpha threshold in the
manifest determines crop bounds only; source alpha is preserved inside each crop.
PSD readback checked all 23 named layers and their decoded pixels. Native Cubism
import and the saved editable project provide the import verification.
