# Local raster image generation

Natural requests such as “paint a portrait of a fox” or “generate an illustration
of a moonlit forest” use the image model. Diagram, flowchart, wireframe and UI
layout requests use the existing structured renderer. `/imagine <prompt>` selects
the raster path explicitly; `/draw <request>` selects diagrams. An unavailable
image runtime reports the missing capability without substituting a diagram.

`Visual/ImageGenerator` owns the authenticated loopback worker and its Windows
kill-on-close process job. `DocumentWorkshop` owns artifact coordination; the
session retains admission, cancellation and publication. The worker admits one
asynchronous job and retains at most sixteen recent job receipts. There is no
background generation service or additional scheduler.

Stop, expired admission and timeout terminate the owned worker, including model
loading or an unresponsive inference call. The Python step callback also supports
cooperative cancellation. Each request uses a unique owned destination. The worker
decodes the temporary PNG before atomic publication; native code independently
checks the exact destination, job/model identity, dimensions, SHA256 and actual
pixel decoding before Canvas publication. Completion verifies file integrity;
prompt satisfaction remains a visual judgment.

## Installation and provider selection

Run `Tools/InstallImageModel.ps1` to install the pinned local Python stack and
SD-Turbo weights. Runtime generation defaults to offline use of installed weights.
The installer is the explicit download path. `image.enabled` enables generation.
Private companions retain their own output directory while sharing installed
weights and tools.

| Setting | SD-Turbo baseline | SDXL-Turbo alternative |
| --- | --- | --- |
| `model` | `stabilityai/sd-turbo` | `stabilityai/sdxl-turbo` |
| `variant` | empty, matching the baseline installer cache | `fp16` |
| `steps` | 4 | 4 |
| `guidance` | 0 | 0 |
| `width`, `height` | 512, 512 | 512, 512 |
| `minimumFreeVramMiB` | 4200 | 9500 |

Install the alternative with
`Tools/InstallImageModel.ps1 -Model stabilityai/sdxl-turbo -Variant fp16`.
The tested fp16-only download contained about 6.94 GB of weights/configuration.
The worker enforces a 9500 MiB model floor for SDXL-Turbo, in addition to the
session resource plan's shared reserve. These are measured planning estimates,
not a reservation against other processes. Automatic placement uses measured free
CUDA memory and otherwise uses CPU; an explicit unavailable or undersized CUDA
selection fails accurately. CPU thread count comes from the existing foreground
resource budget. `keepLoaded` defaults to false, releasing the process and weights
after every native job. `seed` defaults to -1 for random generation; a nonnegative
seed supports repeatable comparisons.

Both Turbo providers use 1–4 steps, zero guidance and no negative prompt, as
documented in their [SD-Turbo](https://huggingface.co/stabilityai/sd-turbo) and
[SDXL-Turbo](https://huggingface.co/stabilityai/sdxl-turbo) model cards. Conventional
Diffusers text-to-image models retain explicitly configured guidance, negative
prompt and step count through the same provider contract. Their memory needs and
quality require separate measurement. The SDXL-Turbo model card permits personal
noncommercial/research use under its linked license; commercial distribution is
outside this test's scope.

## Recorded local acceptance, 2026-10-06

The native Windows client ran the actual Diffusers worker on an RTX 5070 with
512×512 output, four steps and seed 42. There was no concurrent live chat model.
The fixed prompt requested a watercolor red fox with a blue scarf, sitting under
a snowy pine tree, full-body composition and morning light.

| Measurement | SD-Turbo | SDXL-Turbo fp16 |
| --- | ---: | ---: |
| Native request, including startup, imports, verification and release | 11.68 s | 17.01 s |
| Pipeline load and device transfer, excluding Python imports | 4.00 s | 7.45 s |
| Inference | 1.468 s | 2.219 s |
| Peak PyTorch allocated GPU memory | 3096 MiB | 7836 MiB |
| Peak PyTorch reserved GPU memory | 3714 MiB | 9030 MiB |
| Active-generation stop latency | 94.5 ms | 81.2 ms |
| Native restart after cancellation | successful | successful |

Each model's restarted sample was byte-identical to its first sample. This proves
fixed-seed repetition for this installation, not character identity across changed
poses or scenes. SD-Turbo depicted the fox, scarf and snow but cropped the requested
full body. SDXL-Turbo provided a more complete fox/tree composition but omitted the
blue scarf and sitting pose. Neither sample satisfies every prompt constraint.
Concurrent chat/speech throughput and larger resolutions have not been measured.

Preserved local reports and PNGs are under `build/capability-evidence-20261006/art-acceptance-sd-metrics` and
`build/capability-evidence-20261006/art-acceptance-sdxl`; generated images remain local runtime data. The opt-in
`Tests/imageGeneration.live.cpp` driver takes Python, service, cache and output
paths, plus optional model and variant. It generates, stops active inference and
generates again after restart. Ordinary tests do not download weights or use GPUs.

Unit/fixture checks cover classification, corrupt or foreign output, exact-byte
receipts, provider parameters, cancellation, concurrent job refusal, actual unload
state, native restart and stale admission. Reference-image editing and executable
ComfyUI workflows are not exposed by this text-to-image slice.
