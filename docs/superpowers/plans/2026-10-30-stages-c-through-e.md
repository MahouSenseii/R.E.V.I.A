# Remaining companion stages implementation plan

> **For agentic workers:** Use the executing-plans workflow, with narrowly scoped parallel implementation and independent review. Preserve the current actual checkout and all earlier work. Do not stage, commit or push.

**Goal:** Complete the existing master specification's remaining stages in order C, D, E, with actual local end-to-end evidence and explicit unavailable-route limits.

**Architecture:** Extend the existing attachment library, scoped conversation references, admitted turn path and Qt chat for short video. Keep the existing generation provider and normal action authorization; add only installed, qualified generation routes. Exercise the integrated existing companion and repair demonstrated failures.

**Tech stack:** Existing C++20/Qt Widgets, owned cancellable local processes, installed FFmpeg/ffprobe, existing llama.cpp vision pair and Diffusers service.

**Spec:** The user's master implementation specification (attachment b3144ffa-8613-434e-a612-502fdb9f1108), sections 6C–10. The user explicitly confirmed C → D → E on 2026-10-30 and requested continued implementation. The design below resolves routine implementation choices within that scope.

## Global constraints and design

- Work in the actual repository, preserving Stage A/B and personal profile, memory, permissions and quiet settings.
- Keep public contracts in Public/Attachments and implementations in Private/Attachments; Qt presentation in Desktop, checks in Tests, service/setup tools in Tools.
- Stage C starts with one private local MP4/H.264 or WebM/VP8/VP9 clip per turn, maximum 64 MiB, 60 seconds, 1920×1080 (including portrait equivalent by area/long side), with strict container/codec probing and decode time/output bounds.
- Use explicitly installed FFmpeg/ffprobe under ThirdParty/ffmpeg/bin via the existing runtime artifact resolver. Invoke executable/argument arrays through OwnedProcess, never a shell; prohibit network protocols and external references. Missing tools produce an accurate explanation.
- Video bytes remain in the existing private attachment store. References add bounded video metadata; no original path, video bytes or decoded frames go into archives. Forget, scope changes, retry/edit/regenerate and restoration retain the existing admission/lifecycle contracts.
- At most four 768-long-side timestamped frames enter an analysis request; they share the existing image/context budget. Default sampling spans the clip. Timestamp/interval follow-ups request fresh samples in the specified interval. Always state sampled coverage, absent audio evidence and uncertainty about motion between samples.
- Playback uses a separate bounded, silent Qt preview (640×360 maximum, reduced frame rate), with play/pause/seek/stop and no unbounded frame cache. Decoder work stays off the UI thread; closing, forgetting or changing audience/session invalidates it.
- Optional speech transcription reuses the existing recognition owner only if its installed route can be safely applied to a bounded clip interval; otherwise expose the exact missing prerequisite without claiming audio understanding.
- Stage D preserves verified text-to-image. Qualify actual reference editing/image-to-video/text-to-video only for installed supported routes; keep absent model/runtime routes explicitly incomplete. Large model downloads need the user's separate approval, and no cloud or unreviewed nodes are substituted.
- Stage E uses isolated private fixtures and actual model/tool runs. Measure direct-chat and stop latency and GPU/RAM under media workloads; retain ordinary/creative failures as failures. Physical voice tests require real supported playback/capture evidence.

## Review focus

- A timestamp follow-up must select new source pixels rather than describe the poster or prior summary.
- Malformed containers, external references, extreme dimensions, indefinite duration and output floods must fail within bounds and leave no accepted private copy or child process.
- Forgetting or changing scope during decode/playback/model admission must invalidate bytes and presentation without resurrecting derived frames.
- A sparse sample cannot establish continuous motion, complete action coverage or sound; embedded instructions remain untrusted data.
- Self-contained factual video requests use current sampled evidence without older reply templates. Canonical dialogue remains stored, and personal, creative and prior-context questions retain their state.
- Repeated fields at different timestamps are data. Prose cleanup must preserve structured rows, decimal timestamps and requested repeated values.
- Forget clears source access, sent posters, playback links and orphaned document image caches. Counts distinguish uploaded clips from their decoded samples.
- Media load must preserve direct stop/UI responsiveness and the companion's current request and identity.

## Stage C: short-video intake and review

- [x] Add a focused decoder contract and actual local decoder tests for metadata, timestamp selection, bounded PNG sampling, corruption, unsupported formats, cancellation and playback streaming.
- [x] Extend the existing intake/store/reference/archive owners for Video, with codec/duration validation, scoped selection and interval resampling. Add targeted regression checks before implementation and preserve image behavior.
- [x] Connect video evidence through the admitted conversation path and exact frame ordering/labels through the vision request. Exercise replay, restoration, unavailable vision, tampering, forgetting, public/new-chat and hostile media cases.
- [x] Extend attachment chips/transcript/documents view with poster, duration, progress/cancel and a trusted bounded local player; verify attach/drop/send/play/seek/stop through actual Qt.
- [x] Reuse the optional recognition route where installed and qualify separate visual/transcript evidence; accurately report absent audio analysis. Retain actual uncertain ASR word errors explicitly.
- [x] Build affected targets, run relevant existing and new checks, then qualify real silent and audio clips plus timestamp follow-ups with the selected 9B vision pair. Record content-based oracles, payload/frame identity, latency/resources and cleanup. Strict large-label OCR scope remains a model-quality limit.
- [x] Update the existing progress notes and deployment after independent bounded-path acceptance; preserve the full original failed-oracle verdict and ordinary Stage E work.

## Stage D: generation, reference editing and video output

- [x] Inventory existing installed provider/model routes and retain exact missing prerequisites/download sizes.
- [x] Implement and test the smallest qualified reference-editing route in the existing generator/action path, including source digest/scope, cancellation and no silent text-to-image fallback. Actual native and Qt image paths passed; exact visual-edit fidelity remains limited.
- [ ] Implement/qualify each installed supported image-to-video/text-to-video route with independently decoded actual outputs. Retain unavailable model/runtime routes explicitly partial and request only necessary additional large-download authorization.
- [ ] Publish verified generated artifacts in the existing chat and feed them through the Stage B/C understanding path for grounded follow-ups.
- [ ] Run focused generation/privacy/action regressions and actual local acceptance; update progress with precise scope, limitations and evidence.

## Stage E: integrated companion

- [ ] Reproduce current-request drift/creative and ordinary conversation failures from preserved Stage B evidence; use controlled prompt/history studies before changing behavior.
- [ ] Repair demonstrated causes with regression coverage, preserving authored profile and state-dependent conversation behavior.
- [ ] Exercise actual initiative → grounded code comment → follow-up, self-caused error acknowledgment, useful positive feedback, natural media comments, Focus/pause/duplicate/stale gates and voice interruption.
- [ ] Measure direct chat, stop and resource behavior during video processing and generation; repair actual bottlenecks or expose a measured responsiveness/throughput choice.
- [ ] Run affected tests, independent review and actual end-to-end acceptance. Record unverified/blocked requirements honestly, deploy accepted changes, clean up owned processes, and leave an exact continuation checkpoint.
