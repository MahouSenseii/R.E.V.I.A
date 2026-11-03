# READINESS-005: model-free readiness diagnostics

The new diagnostic path exposed a current socket-permission denial before any
model reload. The controller's initial /health requests raised PermissionError,
errno 13 / WinError 10013, while an owned standard-library Python server logged
that it was listening. No HTTP response or readiness gate succeeded.

Microsoft documents [10013 / WSAEACCES](https://learn.microsoft.com/en-us/windows/win32/winsock/windows-sockets-error-codes-2)
as a socket access-permission error. The specific policy component is unknown.
This result is consistent with PREFIX-004's earlier timeout; it does not prove
that historical attempt had the same cause. No model was loaded in this follow-up.

## Confirmed diagnostic defect and narrow repair

The old controller and its nested shared readiness helper both discarded
connection/OS/HTTP exceptions until timeout. The transport boundary now retains
fixed stage labels, exception classes, numeric errno/winerror/HTTP codes, elapsed
time and observed gate booleans in the existing owned-server receipt. At most
32 events are retained: first 16 and most recent 16, plus total/drop counters.
Terminal guard errors preserve the earlier OS error. Metadata observation does
not assume valid nested shapes; the original helper retains rejection behavior.

No bodies, arbitrary exception messages, headers, credentials, private paths,
port or transient PID are retained in public diagnostics. Existing authentication,
model-alias, context/slot, resource, retry and cleanup gates are unchanged. The
shared backend helper and native resource guard are unchanged. This fixes lost
evidence; it does not change Windows/network policy or establish a cache gain.

## Nonmodel validation and owned wire control

Eight new offline controls exercise the actual nested readiness/helper logic
with only transport, clock and timers replaced. They cover permission codes,
inner tokenizer timeout, successful authenticated identity/capacity, rejected
wrong-key/alias/context/slot cases, non-ok health/503, recovery, a fired watchdog,
and malformed metadata. The first six controls failed for absent diagnostics;
the malformed fixture separately caught five observer-shape failures before its
fix. All 23 collector controls and 16 unchanged mocked helper tests now pass.
The first green attempt also caught loss of the OS error beneath a final budget
exception; that failed result is preserved. Default validate-only remains offline.

Independent source review preceded one real nonmodel HTTP control. It used an
isolated Python child, fresh loopback port and ephemeral credential, the existing
one-logical-CPU/2 GiB owned job, 15-second guard interval and one-second HTTP
allowance. Only fake health, tokenizer, inventory and properties endpoints existed;
there was no generation endpoint, llama.cpp executable or model input.

The control stopped after 13.0 seconds as its two-second deadline headroom was
reached. No watchdog fault was recorded. It saw 117 initial-health events;
32 were retained and 85 dropped. Every retained event reports PermissionError,
errno 13 / WinError 10013 and HTTP status null. Health, authentication and identity
booleans remain false; properties and performance remain null. The listening log
is evidence of the child startup, not proof that the client reached its health route.

Owned cleanup was confirmed with no errors, and a later read-only check found the
exact recorded owned PID absent. Its credential directory is empty. Peak process
working set was 21,966,848 bytes and commit was 12,398,592 bytes. These describe
the Python diagnostic fixture, not model inference or memory savings. No automatic
retry followed, limits were not raised, and no firewall/global permissions
or other application was changed. Raw evidence remains private and unchanged;
the [sanitized summary](results/READINESS-005/summary.json) retains its source hash.

## Separately approved permitted-context control

One newly authorized repeat of the same nonmodel control ran outside the
restricted execution sandbox, with no persistent approval or system-setting
change. Source differed only in fresh output location; the controller and
one-CPU/2 GiB/15-second/one-second-HTTP caps were unchanged.

It passed in 0.609 seconds total, with readiness completing in 0.594 seconds.
Six retained events show health success, wrong-key rejection with HTTP 401,
authorized tokenizer success, the exact public fixture alias and context 2048
with one slot. All readiness booleans are true and no transport error remains.
The owned job exited without cleanup errors; its credential was removed and
the exact recorded PID was subsequently absent. Peak process working set was
22,032,384 bytes and commit was 12,517,376 bytes. The original denied control and
its raw hash remain unchanged.

This establishes that the actual readiness gate can reach this synthetic Python
server in the currently permitted execution context. It does not identify the
precise policy component, qualify llama.cpp/Qwen, establish cache/TTFT/model
quality or savings, or retrospectively prove PREFIX-004 had the same cause.
The control duration is not model latency. No model ran in either control.

## Decision and broader research meaning

The separately approved control above passed; no further repeat or model load
follows automatically. A future model readiness-only proposal needs an exact
reviewed command, pinned identities, caps and approval. Preserve the original
failed attempt and do not replay the 21-launch study, change firewall rules or
attach to Revia. The new result qualifies the diagnostic transport path only.

RNL's broader goal remains a trainable adaptive/stateful architecture. Cache
correctness and execution tests qualify one ingredient and the measurement path;
they neither implement that architecture nor rule out other workable approaches.
The earlier positive RNL-003 synthetic result stands. A trainable candidate would
still require explicit learning objectives, data lineage, held-out evaluations,
resource accounting, approval and rollback. No training or application integration
is enabled. Candidate for Revia and Integrated remain None.
