# Revia Live2D Avatar Implementation Plan

> **For agentic workers:** Execute this bounded plan inline with the executing-plans skill and one independent final review.

**Goal:** Create original Revia artwork and a tested presentation adapter for VTube Studio, with an explicit Cubism production handoff.

**Architecture:** The adapter reads the existing version 1 Presence snapshot and injects its own custom tracking inputs over a loopback WebSocket. It cannot call inference, mutate memory, or grant actions. The PNG remains reference art until separated layers and a Cubism export actually exist.

**Tech Stack:** Built-in image generation; transparent PNG; Node.js >=22.12 with built-in WebSocket and node:test; Live2D Cubism / VTube Studio as external model production and rendering tools.

**Spec:** Config/avatar.json, docs/REVIA_CHARACTER_DESIGN.md, Tools/Presence/AVATAR_BRIDGE.md, and the user's request to create the avatar and set it up for Live2D.

## Global constraints

- Preserve authored identity and the existing runtime behavior.
- Keep generated reference art under Assets/Avatar/Revia; model exports remain user data.
- Keep model selection unselected until a real model has been imported and verified.
- Never describe the current speaking gate as audio-amplitude lip sync.
- Require VTube Studio's native plugin approval; never print authentication tokens.
- Use only loopback renderer endpoints. Do not automatically load a different model.

## Review focus

- Missing, corrupt, future or unsupported snapshots must reset inputs.
- Event-only snapshots are not liveness heartbeats; expire transient inputs conservatively.
- Denied/revoked authentication must stop instead of repeatedly requesting approval.
- Disconnects must reject pending requests and release owned inputs on orderly shutdown.
- Input parameter mappings are a separate required VTube Studio configuration step.

## Tasks

- [x] Save and inspect the generated art, record its exact prompt and alpha evidence.
- [x] Add failing checks in Tests/live2dBridge.test.mjs for snapshot admission, mapping, freshness, local endpoints, request correlation, authentication and shutdown.
- [x] Implement Tools/Presence/Live2D state mapper, API client and command-line runner. Use dependency-free Node APIs; expose inspect mode that never connects or writes credentials.
- [x] Add documented layer names, rig parameter mapping and the exact missing Cubism export requirements. Update the bridge and architecture owner documentation.
- [x] Run the focused Node checks and register them with CTest. Review the whole package, merge to main, push, verify remote identity and remove only this package branch.

## Execution record

- Canonical design was already authored; no new personality or character identity was invented.
- The working checkout was clean on main. A short-lived codex/revia-live2d-avatar branch is used in place to retain the existing build fixtures and keep usage bounded.
- Full Cubism production cannot be represented by a flattened PNG or an invented .moc3 file. Layer separation, deformers, physics and actual export remain external production requirements.
- Artwork SHA-256: D0E2EAB036C6E3DFD7F0988A3AA78A2F7C41EEF56376803740047E1F280CAC0D. Verified 1024 × 1536 Format32bppArgb, corner/outside alpha 0, face alpha 253; visually inspected the generated full-body image.
- Director `/root` created the art, adapter and handoff. Read-only supervisor `/root/foundation_supervisor_v2/guidance_worker_v2` reviewed API compatibility and the whole package. Review found old-idle tracking loss and uncancelled pending approval; both received failing regression checks and fixes. Recheck reported no remaining necessary finding.
- Full adapter suite: 13/13 passing. Registered focused CTest checks: Live2DBridge, DiscordVoiceAdapter and SetupScriptPolicy, 3/3 passing. Help/inspect and syntax checks passed. Default live connection reported VTube Studio unavailable; no model/visual acceptance was claimed.
- The affected ReviaDesktop target built successfully. Final focused CTest run added DesktopSmoke: 4/4 passed in 7.98 seconds. Exported Live2D models and local VTube Studio credentials were verified ignored by Git.
- Implementation 21d1a73d2fe70d97a8428cb7bfca76ae81b66fc9 was fast-forwarded to main and pushed. The merged main focused run passed 4/4 in 4.92 seconds. GitHub main matched the exact implementation hash; the merged package branch was removed locally and was absent remotely.
- This bounded art/adapter/handoff package is delivered. The user's complete animated Live2D avatar still requires separately drawn layers, Cubism rigging/export, imported model mappings and actual live visual acceptance. No rig or live-animation completion is claimed.

## Continued production and connection, 2026-10-06

- The owner installed Cubism and VTube Studio and authorized the native Revia Presence plugin. Reference-matched face/backing and mouth patches retain the original source layers and their packaging proofs. The native first rig was exported and installed locally; model binaries and authentication remain ignored user data.
- Director `/root` coordinates runtime, renderer and build acceptance. Worker `/root/audio_lipsync_trace` owns bounded WAV loudness extraction and active-playback admission tests. Supervisor `/root/live2d_bridge_review` reviews the transport, lifecycle and build dependency changes. The work uses the existing Speech → RuntimeEvent → Presence → renderer pipeline.
- The actual renderer exposed a Node built-in WebSocket incompatibility after its first reply. The pinned `ws` transport with compression disabled passed three correlated live API replies. Approved authentication now persists locally; a renderer restart reconnected without another approval and retained the selected Revia model and mappings.
- Eleven visual mappings were applied with a native-settings backup. Live readback and visual checks confirmed mouth, joy, sadness, anger, focus, listening and engagement, plus automatic blink and breath. The production adapter passed controlled source-corrupt, missing, offline, recovery and expired-gate checks against the actual renderer using an isolated source.
- Actual desktop replies and Qwen voice playback ran. The owner confirmed the avatar worked and reported that the original binary lip-sync gate was off. The correction publishes 50 ms quantized RMS windows from the existing Qwen WAV, bounded to 120 seconds, with estimated PlaySound submission timing. Next-phrase generation retains the active audio track; silence and track completion close the mouth. The existing PCM decoder serves both file and memory paths.
- Review identified revoked active playback retaining its mouth track. A neutral `PlaybackEnded` notification closes the matching track without stale private synthesis details or completing a newer conversation intent. Its regression revokes captured admission after the actual prepared Qwen playback starts, using an injected no-audio player.
- Gaze/head X/Y turns, hair physics and independent limbs remain unrigged; blink compression and the single speaking-mouth shape remain a first pass. This is measured audio loudness with estimated timing, not device-cursor timing or phoneme lip shapes. The corrected desktop still requires a fresh real conversation check before claiming live lip-sync acceptance.
- Final Qt MinGW 13.1 desktop build succeeded. The runnable `build/review-5c9cfb2/clean/ReviaDesktop-lipsync.exe` is a hash-matched copy beside the existing runtime, whose Qt/compiler DLLs matched the new deployment; it retains that runtime's user data. The local ignored CLion preset selects the matching compiler and retains debug symbols; the separate test deployment strips debug symbols.
- Fresh affected CTest run passed 6/6: speech admission, playback envelope, playback Presence, existing performance/WAV checks, setup-script policy and Live2D bridge, with the opt-in real renderer transport check enabled. Node ran 30/30 including five setup cases and the live status regression. Active playback revocation reproduced the expected failing assertion before the neutral cleanup fix, then passed all nine admission owner fixtures. Independent final review reported no remaining required finding.
