# Finite companion qualification fixture

This Windows tooling slice uses a standard-library Python loopback provider and two sequential native workers. It exercises production session, memory, correction, identity, routing and reply owners with synthetic extraction. It cannot qualify a real model's extraction, answer quality, performance or continuity. Every receipt states `scope=fixture`, `real_model=not_run`.

Build `ReviaCompanionQualification` using the existing configured foundation/toolchain. No model, dependency installation or desktop is required. Set a fresh synthetic credential in a named environment variable; never put its value on a command line. Then run:

```text
python -B Tools/Quality/qualify_companion.py --fixture --scenario memory-restart --runtime-root <ABSOLUTE-EMPTY-SCRATCH> --provider-port 0 --api-key-env QUAL1_FIXTURE_KEY --worker <ABSOLUTE-NATIVE-EXE> --report <ABSOLUTE-FRESH-REPORT>
```

Port zero requests a newly allocated loopback port. A fixed occupied port is blocked; the runner never attaches to it. Runtime paths and reports must be absolute and cannot cross symlinks/junctions/reparse points. Existing data, reports and phase-evidence folders are refused. Reports must be outside companion data. The runner retains its uniquely owned synthetic files and never recursively deletes data or kills unrelated processes. A denied socket connect is an execution-context blocker; do not change firewall, loopback exemptions or machine permissions to make this fixture pass.

Use a short scratch path. On Windows the runner blocks roots whose critical SQLite journal or atomic identity temporary path reaches 250 characters, leaving room for native file operations without changing the machine's long-path policy. Test case directories use short unique suffixes for the same reason.

The first phase submits an attributed initial project statement and explicit correction, polls bounded owner events, then accepts the genuine proposal using the public owner API. Its durable revision must be observed before normal process exit. The recall phase refuses a missing database/identity, restores no archive tail, introduces only the same synthetic name, and asks the current-label question. The synthetic reply is available only when the actual outbound reference records contain an active owner revision. The runner binds that record's participant, revision receipt and row ID to phase A's durable receipt, checks distinct PIDs and normal exits, and requires relationship continuity. Superseded audit rows are allowed; active/current Cedar evidence is refused.

The recall phase separately exercises public audience routing and a later private positive control. Captured request bodies contain no Authorization headers. Each public outbound string field is checked separately for each private sentinel. Private row snapshots and durable revision counts before/after the public turn must match. This covers the first routing control, not Q02's in-flight revocation/lifecycle campaign.

All optional hosts are disabled in scratch configuration before `Start`: model autostart, cloud, Fast/Expert and warmup, embeddings, reranker/NLI, speech/greeting/recognition, presence/avatar/adapters, vision/perception, initiative/curiosity, barge-in, image/video/performance, improvement, MCP, local API, response review and self-inquiry. Memory stays enabled; archive restore/delegation stay off. Missing companion-local VM configuration disables that host. A Windows job limits the worker to its own process, prohibiting child model/helper launches. Any setup/effective-config mismatch fails before a successful phase receipt.

Local production configuration does not support an environment-name credential field. This fixture deliberately binds the env key into a copied endpoint/profile through a fixture-only friend helper after genuine `Start`; it never copies the key into `session.settings` or a settings file. Metadata GETs are public on the newly owned listener; completion POSTs require the exact bearer. Production Main startup warmup probes occur before this fixture credential binding and can be rejected; they do not certify a warmed model. The fixture does not claim production environment-credential support. Reports scan owned artifacts for credential bytes and redact captured child output, including failure paths.

`--fault` accepts `empty-state`, `rejected-correction`, `failed-save`, `retrieval-disabled` and `interrupt-before-receipt`. They must fail the positive restart oracle. Failed-save holds a fixture-owned SQLite transaction while the production acceptance attempts its durable write. Interruption exits before acceptance without claiming a durable receipt; recall remains not_run. Each control retains separate evidence. Unsupported live modes are blocked with nonzero exit.

Native phases take exactly `--phase`, `--runtime-root`, `--provider-port`, `--api-key-env`, `--receipt`, `--fault`, `--run-id`. The runner owns the run marker and fresh phase paths. `preflight` validates settings without constructing/starting a session; `transport` probes the owned authenticated fixture without `Start`. Normal runner phases are `write-correct` and `recall`; no history or answer injection argument exists.

Protocol checks:

```text
python -B Tools/Quality/qualifyCompanionTests.py
python -B Tools/Quality/qualifyCompanionTests.py --worker <ABSOLUTE-NATIVE-EXE> --evidence-root <ABSOLUTE-FRESH-EVIDENCE-PARENT>
```

CTest registers `Revia.CompanionQualificationProtocol` and `Revia.CompanionQualificationFixture` on Windows with Python available. Both are fixtures; ordinary CTest cannot start a real model. Discover the exact selected registrations before running CTest and use `--no-tests=error`. The native test includes one positive scenario and five discriminating controls; fake-child protocol tests supplement it and cannot replace it. A failed junction-creation prerequisite remains blocked and returns nonzero; `QUAL1_JUNCTION_BLOCKED_EVIDENCE` can retain the known failure location without retrying a denied operation, and does not mark the check passed or skipped.

Version 1 JSON receipts include exact source manifest, worker hash/path, synthetic workload/configuration identities, requested/observed provider identities, phase PID/exit/status/timing, independent check outcomes, outbound bodies and cleanup status. Missing, malformed, duplicate-field, wrong-phase or wrong-identity child receipts cannot pass. Counts retain failed/blocked/skipped/not_run/interrupted outcomes in the denominator. Timings describe this synthetic workload only. Full-suite execution, real models, real desktop/audio/GPU behavior and later qualification scenarios remain separate work.

Native receipt fields are explicitly allowlisted. A child cannot supply runner-owned exit, cleanup, timing, pipe output or credential observations; unexpected fields reject the receipt. Raw receipt bytes are checked and sanitized before UTF-8 decoding, JSON parsing or identity validation, so rejected or undecodable evidence cannot retain the fixture key. Pipe and phase-receipt credential observations are separate. A sanitization write failure remains a failed phase with explicit failure evidence.

Accepted provider sockets have a finite idle-read timeout and are tracked by the listener that owns them. Context exit closes those accepted connections before requesting shutdown. Shutdown runs in an owned helper thread with a bounded join, followed by a bounded listener join; cleanup is reported closed only when both threads and accepted requests have finished. Otherwise cleanup remains uncertain and qualification fails. Partial request lines and authenticated incomplete bodies are tested in sole owned fixture children with hard parent timeouts, so the regression test itself cannot leave a hanging provider behind.
