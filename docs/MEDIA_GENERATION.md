# Local media generation

R.E.V.I.A uses its existing image provider and action authorization for generation.
The chat carries host-issued references to verified library artifacts. A path or
receipt supplied by the language model cannot become a generated attachment.
Generated files can be inspected through the same private image/video intake and
follow-up path as uploads. Source text inside media remains untrusted data.

## Images and reference variations

The installed SD-Turbo route produces real PNG files. A request such as
“Draw an anime-style traveler in a blue coat on a grassy hill” uses text-to-image.
“Edit the image you just generated with warmer evening colors” selects a current
stored still image and passes its verified pixels to image-to-image. An ambiguous,
withdrawn or missing reference produces an explanation rather than starting an
unrelated text-to-image job.

The current conservative preset is 512×512 and four steps. Reference variations
share the installed pipeline weights, with bounded source preprocessing and a
nonzero denoising schedule. The receipt records source identity, preprocessing,
seed, effective steps and output digest. The model can change clothing, faces,
colors or composition substantially; this route is not precise inpainting or
character-preserving editing.

One actual local anime-style qualification produced a 512×512 illustrated PNG in
29.0 seconds including loading (1.45 seconds diffusion). It missed requested hair
and prop details. Basic anime-style bitmap generation is supported; polished
anime quality and consistent characters have not been established.

## Short video

The optional native worker uses the approved pinned LTX-Video 0.9.5 assets. It
supports text-to-video and a single current still image as image-to-video input.
Editing an uploaded video is unsupported and cannot fall back to text-to-video.
At the D5 wrap checkpoint the final native/Qt video path is still unqualified;
the route remains disabled in normal settings.

Examples are “Make a short video of a red ball rolling across a wooden table” and
“Animate this image with a gentle breeze and a fixed camera.” Video runs as one
owned background task. Ordinary chat remains available. `/video status` reports
availability and current state; `/video cancel` requests cancellation and the
terminal task report confirms when the worker actually stops.

Video defaults are disabled. The `video` section of `Config/settings.json` contains
the existing Python environment, worker and decoder paths. This machine's approved
assets are in `D:/ReviaModels/LTX-Video-0.9.5-e58e28c`; its isolated runtime overlay
is `_runtime/site-packages` under that directory. The `modelDirectory` and
`runtimeOverlayDirectory` settings must name those installed assets before the
owner enables `video.enabled` and restarts. No generation request downloads models
or invokes a cloud fallback.

The conservative local preset uses the secondary GPU, CPU offload, 384×256,
49 frames at 25 fps, 50 steps and a 600-second job limit. This produces about
1.96 seconds of silent video. The worker records the actual seed and resource
checkpoints. It verifies dimensions, duration, frame count, file digest and every
decoded frame before publishing a result. These mechanical checks do not judge
visual quality.

Separate actual model trials took about 153–165 seconds per clip, with Main chat
turns around 1.4–1.6 seconds on the tested configuration. Requested objects can be
unclear and source details drift after the initial frame. Faithful animation,
continuous character identity and general video quality remain limitations.
Those trials did not measure Torch process-tree memory: their corrected review
labels launcher measurements accurately. The application worker reports its own
process RSS and available-system-memory checkpoints, with process-tree memory
explicitly marked unmeasured.

## Privacy and lifetime

The canonical library owns the verified result; the import removes the old owned
provider output after a successful transfer. Assistant messages and archive
associations retain bounded identities and provenance rather than embedding raw
media bytes or source paths. Follow-up inspection re-opens the current bytes.
Private video scratch uses validated local TEMP outside known OneDrive roots.
The final long-running qualification of that scratch change remains pending.

Forget retires the logical identity even if its file was removed or tampered with.
An operational cleanup failure is reported rather than reviving that identity.
New Chat, participant/audience changes, stop and source withdrawal invalidate
pending publication. Quiet mode suppresses speech while preserving an admitted
completed result. A generated clip has a trusted local player link; its card does
not currently display a video thumbnail.

The progress notes distinguish provider, library, native chat and Qt acceptance.
An isolated provider output or passing fixture is not proof that the complete
application path has passed.
