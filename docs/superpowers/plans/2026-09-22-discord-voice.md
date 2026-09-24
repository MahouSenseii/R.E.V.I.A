# Selected microphone and Discord voice implementation plan

**Goal:** Correct hands-free microphone selection and connect Discord voice to the existing Revia public conversation runtime.

**Architecture:** A manually started, single-channel Node connector owns Discord credentials, DAVE/Opus transport, speaker attribution, and bounded in-memory capture. It calls the existing local Whisper server and exchanges correlated Presence envelopes with Revia. Revia renders only its final public reply through the active profile's existing Qwen pool; the connector never contains an LLM, identity, memory, or desktop-action implementation.

**Constraints:** Discord input remains public conversation. Stable platform IDs identify speakers. Private local memories and desktop context remain excluded by ReplyPublic. No Windows playback for Discord voice. No automatic joining during installation or tests. Name-addressed replies are the default; participation is a local connector setting. Bot tokens come from the environment only. Node >=22.12, discord.js 14.27.0, @discordjs/voice 0.19.2 with its DAVE dependency.

## Tasks

- [x] Resolve the microphone in CaptureHandsFree exactly as in manual capture, including fallback and device-specific diagnostics. Validate existing microphone selection tests and the native build.
- [x] Extend Presence with an optional Discord voice delivery request and bounded audio reply files. Add render-only speech using the active Qwen voice, preserving the existing public reply path. Test parsing, publication, failure without an assigned voice, and external command isolation.
- [x] Add a DiscordVoiceAdapter connector with bounded capture, one serialized conversation queue, speaker/channel checks, timeout/cancellation, strict reply correlation, and loopback-only Whisper. Test with synthetic PCM, disposable inbox/outbox, and fake transcription/transport; no Discord account or microphone required.
- [x] Document local setup, explicit startup/shutdown, privacy boundary, retention, and live-test limitations. Register Node tests in CTest, build, run relevant checks, and review the final diff.

## Review focus

- Wrong/stale channel or speaker replies must never play.
- Sustained speech, excessive concurrent speakers, missing replies, and worker failure must stay bounded.
- Shutdown/disconnection must cancel pending work and prevent late playback.
- An external request to delete files must remain a public conversation turn.
- Missing Qwen voice must fail visibly without fallback to Windows speakers.

## Verification record

- Native build: `cmake --build build/review-ci --parallel 4` passed with MinGW 13.1 / Qt 6.8.3. Final `ctest --test-dir build/review-ci --output-on-failure --timeout 600` passed all 8 suites in 82.24 seconds, including Foundation (74.87 seconds), DiscordVoiceAdapter, and DesktopSmoke. Full log: `build/review-ci/Testing/Temporary/LastTest.log`.
- `npm test` in `Tools/Presence/DiscordVoice`: 18/18 passed, including a loopback Whisper multipart request and installed Opus encode/decode.
- `node main.mjs <disposable-config> --check`: passed without opening a Discord connection; DAVE, Opus, and XChaCha dependencies loaded.
- Focused native executable using current compiled source: Discord envelope parsing/rejection, audio retention/publication, active-profile Qwen request, mute, worker shutdown, and existing microphone suites passed.
- `ReviaTests.exe --speaker-continuity`: passed through the real session adapter queue and public conversation path, including the Discord voice request to delete files.
- CI workflow YAML parsed successfully. Added a dedicated offline Discord transport job; no hosted run has been triggered by this task.
- Independent code review identified three lifecycle issues. Speaker departure now aborts accepted and queued turns; the startup signal owns a connection before the caller resumes; session shutdown latches the Qwen pool before joining adapter renderers. Regression tests cover all three.
- Verification caught a test-fixture teardown error: mock occupied voice-worker slots were not released. A debugger confirmed the wait in pool cleanup; releasing the mock slots fixed the fixture and the focused suites passed.
- No live Discord channel or GPU voice-generation test was performed. Activation requires local bot/channel configuration and running local Whisper/Qwen services.
