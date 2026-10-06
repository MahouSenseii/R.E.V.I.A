# Revia reference-aligned backing v2

This package replaces only `Face_Underpaint` with pixels from the generated edit
of the approved reference. The original `revia-parts.psd`, source reference,
layout, preview and all 25 individual part PNGs remain unchanged.

| File | Purpose |
| --- | --- |
| `revia-parts-reference-backing-v2.psd` | Full 1024 × 1536 PSD with the replacement backing and all 24 other layers preserved. |
| `revia-underpaint-only-v2.psd` | One-layer 1024 × 1536 PSD for replacing the native backing input image. |
| `Face_Underpaint.png` | Cropped 128 × 125 replacement pixels. |
| `Face_Underpaint-canvas.png` | Replacement registered on the transparent 1024 × 1536 model canvas. |
| `proof.json` | Source/output hashes, coverage counts and preservation checks. |
| `imagegen-prompt.txt`, `provenance.json` | Exact root-supplied generation prompt and source/assembly provenance. |

The replacement layer is at **left 449, top 305, width 128, height 125**. It is
hidden in both PSDs, matching the original neutral visibility. The one-layer PSD
therefore has a transparent composite; its backing pixels are stored in the
named layer. Native runtime visibility is a rigging decision.

The head transform is unchanged: crop the 1024 × 1536 generated reference at
`[340, 0, 350, 310]`, scale by `0.8`, and place the resulting 280 × 248 head at
`[374, 232]`. The mask combines the layout's Face, eye-white, upper/lower eyelid,
brow and upper-mouth polygons with a four-source-pixel safety margin (3.2 model
pixels). Alpha is the minimum of the approved-reference alpha and generated-edit
alpha. All 1,579 opaque eye, iris, eyelid, brow and mouth source pixels have
opaque backing beneath them. Enlarged visual inspection shows no new ear at the
viewer-left eye.

The full PSD preserves the decoded pixels and complete parsed metadata of the
other 24 layers exactly, including order, names, bounds and visibility. Encoding
the composite through PSD white-matte rounding changes weighted RGB by at most
1.326/255 and leaves its alpha exact. This is not a byte-identical copy of the
entire original PSD.

In Cubism, replace only the `Face_Underpaint` input image while keeping its
existing rig objects and parameter assignments. The backing bounds are larger
than the original `[455, 308, 116, 118]`; inspect its ArtMesh coverage and adjust
only that mesh if necessary. Confirm neutral appearance and closed-eye coverage
in the native model before export. The PSD verification does not validate
deformed mesh coverage or animation behavior.
