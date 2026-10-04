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
- [ ] Run the focused Node checks and register them with CTest. Review the whole package, merge to main, push, verify remote identity and remove only this package branch.

## Execution record

- Canonical design was already authored; no new personality or character identity was invented.
- The working checkout was clean on main. A short-lived codex/revia-live2d-avatar branch is used in place to retain the existing build fixtures and keep usage bounded.
- Full Cubism production cannot be represented by a flattened PNG or an invented .moc3 file. Layer separation, deformers, physics and actual export remain external production requirements.
- Artwork SHA-256: D0E2EAB036C6E3DFD7F0988A3AA78A2F7C41EEF56376803740047E1F280CAC0D. Verified 1024 × 1536 Format32bppArgb, corner/outside alpha 0, face alpha 253; visually inspected the generated full-body image.
- Director `/root` created the art, adapter and handoff. Read-only supervisor `/root/foundation_supervisor_v2/guidance_worker_v2` reviewed API compatibility and the whole package. Review found old-idle tracking loss and uncancelled pending approval; both received failing regression checks and fixes. Recheck reported no remaining necessary finding.
- Full adapter suite: 13/13 passing. Registered focused CTest checks: Live2DBridge, DiscordVoiceAdapter and SetupScriptPolicy, 3/3 passing. Help/inspect and syntax checks passed. Default live connection reported VTube Studio unavailable; no model/visual acceptance was claimed.
- The affected ReviaDesktop target built successfully. Final focused CTest run added DesktopSmoke: 4/4 passed in 7.98 seconds. Exported Live2D models and local VTube Studio credentials were verified ignored by Git.
