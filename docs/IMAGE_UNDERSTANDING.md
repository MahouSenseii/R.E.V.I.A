# Uploaded image understanding

Image understanding uses the existing private attachment library and local llama.cpp conversation route. Uploading an image does not enable screen, camera, microphone, network or general filesystem access.

## Intake and retained content

The chat attach button, file drop and clipboard paste use the same Desktop intake worker. Qt decodes supported image formats, scales large images to the configured Desktop image limit and stores a PNG copy in the companion's existing attachment directory. Raw core intake accepts bounded PNG/JPEG/GIF content; WebP requires Desktop conversion. GIF and animated WebP conversion is still-image inspection, not animation review.

The current Desktop limit is 1,024 pixels on the longer side. Core limits are 8 MiB per image, 2,048 pixels per side and four images per turn. Only the accepted private copy is retained. The original selected file is not reopened later, and detail discarded during resizing cannot be recovered. Small or illegible text requires a readable crop or a clearer upload rather than an invented transcription.

Each original user message retains ordered attachment IDs, content digests and image metadata. Conversation history and its archive retain no image bytes or original filesystem paths. Each reinspection resolves the authorized IDs through that companion's current attachment store, checks its owned path, byte limit, digest and dimensions, and puts freshly read pixels only on the current model request.

## Conversation continuity

Follow-ups can select a numbered image, an unambiguous filename, the current image or a comparison with the preceding version. Ambiguous identity produces a clarification request. Ordinary unrelated conversation sends no images. The four-image and model-context budgets remain in force; a reference is not permission to send the entire library.

Each image in a multi-image request has a separate neutral text marker. The
installed Qwen-VL backend otherwise fuses consecutive equal-sized images as
temporal frames, which caused incorrect comparisons in an actual run. This
[upstream issue](https://github.com/ggml-org/llama.cpp/issues/24303) matches the
captured one-chunk server trace. Text inside uploaded images remains untrusted
reference content, including apparent instructions or claims that a tool ran.

Edit, Regenerate and Retry preserve the valid associations of the replaced user turn. Explicit continuation of a conversation saved with media references restores its scoped associations. Legacy text-only archive rows remain readable without inventing missing image associations. Starting a new chat does not grant that chat the prior chat's images. Private participant, companion and audience checks apply before retrieval and again through reply admission.

Forgetting a stored image deletes its private copy and library record, revokes pending dependent replies and clears associated prior image analysis from conversation context/archive. If physical deletion fails after removal of the library record, the operation reports that partial failure and still invalidates derived analysis. Opaque unavailable-reference metadata preserves image numbering; it cannot reopen or recreate forgotten bytes. Image turns do not enter automatic durable memory classification.

## Availability and generation

Uploaded pixels require a local model with a working matching projector. `llm.visionEnabled` is the main-model control; `vision.enabled` controls separate screen awareness. The owner setting overlay can override `Config/settings.json`, so effective settings and the backend's reported vision capability must both be checked.

`Tools/SelectInstalledMainModel.ps1` provides a narrow setup path for an already
installed, manifest-pinned model/projector pair. Its `DryRun` retains original
bytes, the proposed three Main model string values and asset fingerprints;
`Apply` requires that unchanged receipt and retains a pre-apply backup. Other
settings bytes, the authored profile, owner choices and vision flags are
preserved. Selection does not establish visual accuracy or enable vision. Stop
the app and other configuration writers before applying a reviewed selection.

`llm.imageMinTokens` controls the owned backend's minimum image detail budget;
zero leaves its default unchanged. Supported owner-control presets are 0, 256,
512, 1,024, 2,048 and 4,096, and changes require a model restart. The current Main
configuration uses 1,024 after controlled tests exposed OCR errors at the backend
default. Prompt accounting reserves the corresponding aspect-rounded image cost.
The authored conversation temperature is preserved. A model still may misread an
image; this setting and the bounded acceptance fixtures do not certify universal OCR.

For factual inspection with admitted image bytes, the request uses temperature
zero without the conversational DRY repetition settings. Its final prompt keeps
the authored identity, recent dialogue and actual reference evidence, while
omitting the extra conversational posture and asking for the requested facts
without invented detail. The safely prepared current question follows this
guidance; the stored user message and prior dialogue remain unchanged. The shared
purpose check includes direct naming,
transcription, location and comparison requests. Creative, personal and explicit
prior-dialogue constraint requests retain the full conversational state and saved
sampling choices. This changes an individual inspection request, not the saved
profile, memories, ordinary conversation temperature or access permissions.

Missing vision or unreadable/changed stored images are explained without visual guesses. A previous textual description is distinct from a fresh pixel inspection.

The attachment owner supplies ambiguity, missing-image, admitted-image-count and
shared-audience explanations directly. An admitted count describes the images
currently available for a request; counting objects inside a picture still needs
visual inference. When validated pixels are already supplied, the visual
reply uses direct inference with explicit uploaded-image availability guidance.
Native commands and the existing ordinary text/document tool path retain their
own behavior. Image guidance is counted with the prompt text before context
fitting, and pixels remain a separate reserved cost.

Existing text-to-image generation keeps its provider and authorization path. Reference-image editing or animation must use a supported reference workflow; the current unavailable route explains its limitation instead of substituting an unrelated text-to-image result. Video understanding and generation are later stages.

## Verification

Stage B acceptance evidence is collected under the ignored `build/stage-b-20261029` directory. The final progress checkpoint records the exact builds, deterministic checks, real model runs, Desktop handler checks, generation regression and outstanding limits. Transport tests and screenshots alone do not establish visual comprehension.

Stage B's bounded upload and factual follow-up path is verified. The final approved
9B model/projector cohort passed the required media oracles in all 19 native
workflows and the three-turn actual Qt path. Fourteen selected checks passed;
the inventory of 127 registered checks was not all executed. Earlier failed
cohorts remain failed evidence. Normal Main selects the matching LMStudio 9B pair,
and the supported owner overlay enables `llm.visionEnabled`; the base vision
default remains false. `Tools/RunDesktop.ps1` uses that explicit runtime root and
owner setting. Screen observation and the authored profile remain unchanged.

The qualification does not certify every extra assertion or ordinary/creative
conversation. Recorded limits include a surplus reply to an older question,
approximate geometry, speculative label meanings and personal follow-up topic
drift. Pre-send screenshots preceded queued chip layout; accepted chip/staged
counts and exact transmitted pixels establish intake, while those screenshots
do not establish complete visible-chip rendering. See
[the progress notes](COMPANION_UPGRADE_PROGRESS.md) for measured latency, exact
evidence, normal deployment and remaining later-stage work.
