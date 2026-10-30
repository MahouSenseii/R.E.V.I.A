# Local short-video understanding

This extends the existing private attachment, scoped conversation and vision
owners. Stage C's bounded sampled-frame path passed native/Qt acceptance and is
deployed. See [the progress notes](COMPANION_UPGRADE_PROGRESS.md) for
the current verified checkpoint and retained failures.

## Intake and evidence

Attach or drop one local MP4/H.264 or WebM/VP8/VP9 clip per turn, separately from
still-image uploads. The initial limits are 64 MiB, 60 seconds, 1920 pixels on
the longest side, 2,073,600 pixels, 1–120 frames per second and 7,200 source
frames. One optional AAC, Opus or Vorbis audio stream is accepted. Invalid,
unsupported, oversized or cancelled intake removes its new private copy.

The attachment library keeps a verified private copy. It checks content identity
before and after inspection; changing or deleting the original file does not
change the accepted attachment. A changed, missing or withdrawn private copy
cannot be reinspected. Chat and archive references retain IDs, digests, dimensions,
duration and requested interval; they do not contain video bytes, decoded frames
or the user's original path.

Four actual source frames, at most 768 pixels on the longest side, share the
existing vision/context budget. Default inspection samples across the clip.
Follow-ups such as `Inspect 00:12.000 to 00:12.500 in that clip` decode fresh
frames in that interval. Each frame's label uses its decoded presentation time,
which can differ from the requested time. Samples at the same source frame are
deduplicated. Every released sample remains inside the selected interval. If no
actual frame lies inside a very narrow interval, the owner explains that limit
and requests a wider interval rather than substituting an outside frame.
Interval limits are checked against the clip's known duration.

Sparse still samples do not establish continuous motion, foot sliding, exact
event onset or duration, or everything that occurred between frames. The native
sampling policy answers a bounded subset of pure motion-confidence questions
from actual coverage metadata. Other visual questions go to the vision owner
with the same uncertainty rule. Neither path is a general action-recognition
or full-video model.

Self-contained factual frame questions use a bounded local-model observation
schema. The host supplies the timestamps and renders only complete, validated
observations; unfinished or malformed output does not become a partial review.
Requested region fields can distinguish the image center from the center of an
object group. Current evidence replaces older dialogue only in this narrow
provider request; the canonical conversation and archive remain intact. Creative,
personal, prior-answer and still-image requests retain their normal context.
Schema validity does not establish that an observation is true. The retained
hostile-text trial recognized the required label and shapes but also quoted small
text outside an "only the large label" request. Precise font-size text selection
remains a model-quality limitation; quoted media instructions confer no authority.

## Local dependencies and playback

`VideoDecoder` uses explicitly installed `ffmpeg.exe` and `ffprobe.exe` under
`ThirdParty/ffmpeg/bin`, through the existing owned-process owner. The optional
[installer](../Tools/InstallVideoDecoder.ps1) copies selected local tools and
their accompanying licenses; it supports a preview and hash receipt. Decoder
tools are user-installed runtime data, rather than checked-in executables.
Missing tools produce an unavailable result. Standalone optional decoder checks
report CTest skip code 77 when tools are absent.

Processes use explicit argument arrays, local-file protocols, disabled MP4
external data references, bounded output/allocation/time and one decoder thread.
Sampling shares a 15-second deadline. These bounds are not a total process RSS
cap. Closing, cancelling or withdrawing access terminates owned work.

Trusted video cards open the bounded silent player. It decodes at most 640×360
at 15 frames per second, with a three-frame presentation queue. Play, pause,
seek, stop and closing do not retain a full decoded clip. A scope/session/media
revision change invalidates the preview. Playback deliberately has no audio.

## Optional uploaded speech

An explicit transcription request extracts bounded 16 kHz mono PCM from the
selected interval and calls the existing installed whisper.cpp CLI/model on
two CPU threads. Uploaded transcription additionally requires the small local
Silero VAD model at `Models/ggml-silero-v5.1.2.bin`; its absence is reported rather
than silently disabling speech detection. The qualified file is 885,098 bytes,
SHA256 `29940d98d42b91fbd05ce489f3ecf7c72f0a42f027e4875919a28fb4c04ea2cf`,
from ggml-org's pinned `60cbe3094451f5bb86ecd0307c814356da24cdf6` release.
This is a separate file operation; it does not start microphone
capture, the normal recognition service or speaker enrollment. Temporary PCM,
immutable CLI input and transcript result files are removed after use.

Transcript segments retain their reported times with the requested clip offset.
They are uncertain ASR hypotheses, separate from visual evidence. Visible words
and spoken words can differ. Non-speech annotations do not establish speech,
and no general sound analysis is provided. Recognition quality remains subject
to actual model qualification; the retained initial trials include a tone-only
false positive and a short-interval word error. Do not treat a valid transcript
file as proof of word accuracy. Native VAD suppressed the tone-only false
positive in qualification; it did not resolve the retained proper-name error.

A narrowly recognized transcript-only request returns the timestamped native ASR
hypothesis directly, without a second conversational model rewriting its words.
Mixed visual/audio questions keep the evidence streams separate through the
ordinary path. An explicit terminal instruction not to transcribe audio leaves a
visual-only request on the bounded frame path. Empty recognition means no clear
speech was recognized, rather than proof of silence. Actual qualification still
misheard "Quartz" as "Courts"; the application preserves the uncertain hypothesis
instead of substituting expected fixture words.

## Lifecycle and verification

The existing private participant/audience admission, archive restoration, replay,
new-chat separation, cancellation, source withdrawal and forget contracts apply.
Public/shared turns do not gain private attachment access. Reinspection verifies
fresh source bytes rather than treating an earlier assistant summary as evidence.

Component checks include `ReviaVideoSelectionTests`, `ReviaVideoCoverageTests`,
`ReviaVideoDecoderTests`, `ReviaVideoAttachmentTests`,
`ReviaVideoFrameReplyTests`, `ReviaFileTranscriptionTests` and `Revia.VideoPreview`. The manual
`ReviaVideoStoreLive` check additionally exercises actual private storage and
decoded pixel/timestamp/lifecycle oracles. Native/Qt acceptance must independently
check the actual model payloads and answers; passing the component checks alone
does not qualify the complete companion path.

CTest's normal wrapper is strict about skipped checks. Windows CI explicitly
allows only the two named optional decoder/preview checks to report their exact
missing-prerequisite return-code-77 result. It records them as unverified optional
checks, with their reasons; inventory discovery is separate from execution.
