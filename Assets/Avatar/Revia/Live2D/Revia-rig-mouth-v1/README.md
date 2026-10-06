# Revia natural speaking mouth v1

`revia-mouth-inside-only-v1.psd` replaces the legacy fang-mouth source with a
modest rounded speaking mouth generated from the approved reference. It contains
exactly one hidden `Mouth_Inside` layer on a 1024 × 1536 canvas, at **left 496, top
396, width 30, height 18**. Its composite is transparent because the layer is
hidden. Native rig visibility remains controlled by the mouth parameter.

`Mouth_Inside.png` contains the same cropped pixels;
`Mouth_Inside-canvas.png` registers them on the transparent full model canvas.
The rectangle includes neighboring generated skin to preserve the full lip.
The visible opening is approximately 18 × 12 model pixels, with no teeth or
fangs. Its 30 × 18 rectangle is centered at `[511, 405]`; the height extends the
initial 30 × 12 target to keep the lower lip and skin margin intact.

The selected full edit is saved as `generated-reference.png`. Only its mouth
rectangle enters the replacement layer. It is sampled with the existing source
head crop `[340, 0, 350, 310]`, uniformly resized to 280 × 248 with Lanczos3, then
extracted at head-local `[122, 164, 30, 18]` and registered relative to `[374, 232]`.
There is no separate mouth stretching, skin color keying, drawn contour or pixel
painting in the assembly.

`imagegen-prompt.txt` is the exact prompt used for the selected second render.
`proof.json` records its hash, the generated image's measured alpha, extraction
coordinates, output hashes and 37 preexisting image/PSD/layout hashes. The
original neutral PSD, source reference, layout, all original parts and the
backing package remain unchanged.

Replace only the native `Mouth_Inside` input image, keeping its rig objects and
parameter assignments. Its bounds differ from the legacy `[503, 394, 37, 37]`, so
inspect the ArtMesh coverage after replacement. Preview the closed/half/open
mouth keys against the face backing before export, including the skin-border
blend and position. Source packaging checks do not validate native deformation
or opacity animation.
