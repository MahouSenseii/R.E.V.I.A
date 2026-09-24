# Optional public text conversations

Revia's public web source is independent of Discord and disabled by default. The existing Portfolio remains a static website whether this PC is running or not. Its chat UI is disabled until a real relay URL is configured. Transport, Render deployment, Docker packaging, rate limits, proxy trust, protocol, threat model and recovery instructions are in [the transport guide](../Tools/Presence/WebDemo/README.md). Verification is recorded separately in [WEB_DEMO_EVIDENCE.md](WEB_DEMO_EVIDENCE.md).

## Native boundary and audit

The pre-existing instance `ConversationRuntime::ReplyPublic` restricts local history and memory but still reads shared mood causes, preferences and humanization state. The existing adapter worker also updates relationships and publishes transcript events. The web source does not use that worker or its instance-state path.

`WebGuestRuntime` owns a separate `messageRouter`, fixed `PublicGuestProfile`, and bounded in-memory guest histories. It passes only model endpoint configuration from the owner runtime. The stateless overload of `ReplyPublic` calls the existing turn coordinator, style policy and completed-response filters with `PrivateMemoryAccess::Denied`, no memory evaluation, no deltas, and explicit unavailable desktop/internet capabilities. The curated profile preserves Revia's curious, playful, expressive character without loading private profile prompts, earned preferences, biography, mood causes, interests or thoughts. No speech is generated.

There is no `ReviaSession::Submit`, command/action dispatch, permission mutation, screen/camera capture, file parameter, arbitrary URL, relationship store, owner history, event bus or transcript logger in this boundary. Constructor types deliberately do not accept those capabilities. Model HTTP uses the existing local inference stack. Request-scoped cancellation now covers preliminary model discovery as well as model-slot waiting and streaming. Final publication checks cancellation, session existence, owner state and deadline again while holding only the short lifecycle mutex.

Owner input preempts the guest before waiting for the desktop operation lock. Beginning any owner operation also preempts the guest, and the guest maintenance worker observes the owner's busy flag. Guest cancellation never calls the global owner Stop operation. One native web turn runs at once; a native refusal is returned immediately to the connector. The relay owns the four-place waiting queue.

## Threat model

| Threat or trust boundary | Enforcement and remaining limit |
|---|---|
| Visitor forges an owner role, tool, filename or URL | Exact versioned schemas refuse unknown authority fields; native accepts only text and trusted session identity. No desktop executor is reachable from guest routing. |
| Prompt asks for owner memory or another guest's history | The separate router receives a fixed public persona and only that guest's bounded history. Private providers and stores are absent; canary tests inspect actual serialized requests. |
| Guest steals or guesses another request identity | Random session bearers are hashed at the relay; each request is authorized against its owning session. IDs alone confer no access. Bearer theft still permits that session until revocation/expiry. |
| Browser page attempts local API access | Listener binds loopback, requires an independent high-entropy bearer and rejects every browser Origin header. This does not sandbox other privileged local processes. |
| Flooding, slow clients or expensive inference | Invite admission, bounded HTTP/WS bodies/connections, source/session limits, one active turn/four waiting, native generation cap and absolute deadlines. Distributed abuse with a leaked invite and compromised infrastructure remain operational risks. |
| Lost responses, stale host sockets or cancellation races | Idempotency hashes, strict epoch/session/request binding, native replay tombstones, acknowledged cancellation plus original-turn unwind, and final publication checks. Restarts intentionally discard sessions. |
| Model output contains HTML, reasoning or diagnostics | Only the completed filtered public answer crosses native; frontend inserts literal text. The model can still produce mistaken or undesirable ordinary language. |
| Compromised relay or network observer | Production uses HTTPS/WSS and certificate validation; relay still sees text and can exercise the narrow conversation contract. Native independently validates and isolates it. Public TLS/proxy deployment must be checked before activation. |
| Operator, host provider or local malware observes data | Application retention is bounded in memory; separate process memory, model service, infrastructure logs and crash tooling remain outside this promise. Use non-sensitive demo text. |

The boundary protects owner state and capabilities through code and separate data paths, rather than relying on a prompt instruction. It is not a security guarantee against compromise of the native process, OS, local model server or hosting account.

## Build and opt in on Windows

Build the current source using the existing setup/toolchain workflow:

```powershell
.\Tools\Build.ps1
```

Supply `REVIA_WEB_LOCAL_TOKEN` through your local secret manager as an independent high-entropy printable ASCII secret of 32–256 characters. It must differ from the relay host credential. Never put either value into the repository. The connector and native process need the same local token in their environment.

In the shell used to start the freshly built native application:

```powershell
$env:REVIA_WEB_ENABLED = '1'
$env:REVIA_WEB_PORT = '17864'
# REVIA_WEB_LOCAL_TOKEN is already supplied securely in this process environment.
.\build\debug\ReviaDesktop.exe
```

Without `REVIA_WEB_ENABLED=1`, no native web listener starts. Missing/invalid credentials or a port conflict leave the web source disabled and report only a generic owner-facing diagnostic. This setting is unrelated to Discord. Changes to model configuration or credentials require restarting the native process and connector. Revia's configured model endpoint must be loopback; no guest can select it. Start Revia's model normally; the local status reports `starting` until its model identity/readiness has been checked, then `online`, `busy`, or `unavailable`. No directory-existence readiness guess is used.

Check native readiness using the local token without printing it:

```powershell
$webHeaders = @{ Authorization = "Bearer $env:REVIA_WEB_LOCAL_TOKEN" }
$webBase = "http://127.0.0.1:$env:REVIA_WEB_PORT"
Invoke-RestMethod "$webBase/web/v1/status" -Headers $webHeaders
```

Pause guest access and cancel its active turn:

```powershell
Invoke-RestMethod "$webBase/web/v1/control" -Headers $webHeaders -Method Post `
  -ContentType 'application/json' -Body '{"enabled":true,"paused":true}'
```

Resume:

```powershell
Invoke-RestMethod "$webBase/web/v1/control" -Headers $webHeaders -Method Post `
  -ContentType 'application/json' -Body '{"enabled":true,"paused":false}'
```

Pause clears native guest histories and ends those contexts. Visitors start fresh sessions after resume. To turn the demo off, disable the native source:

```powershell
Invoke-RestMethod "$webBase/web/v1/control" -Headers $webHeaders -Method Post `
  -ContentType 'application/json' -Body '{"enabled":false,"paused":false}'
```

Disabled native status is `offline`; the connector stops work and reconnecting when it observes that authoritative state. Re-enabling requires enabling native guest mode and explicitly restarting the connector. Pause keeps the connector connected. Ctrl+C also stops the connector immediately. Unset `REVIA_WEB_ENABLED` before the next application launch if it should remain off. Native shutdown also cancels guest work and clears its state. No router port forwarding or inbound firewall change is needed. Local HTTP accepts a separate bearer and refuses browser Origin headers. The connector forwards no control endpoint; these owner commands stay on the PC.

## Native retention and protocol

`127.0.0.1:17864` is the default authenticated native listener. Its routes are `/web/v1/status`, `/turn`, `/cancel`, `/end` and owner-only `/control`. Versioned request fields match [the canonical transport protocol](../Tools/Presence/WebDemo/protocol/README.md). Invalid JSON, unknown fields, forged identity/roles, excessive text and noncanonical UUIDs are refused before inference. Request deadlines are absolute epoch milliseconds; synchronize the PC's clock. A bounded five-second future-clock tolerance never extends native execution beyond 120 seconds.

Guest text/history never uses file IPC or transcript archives. Each live native context keeps at most eight messages and 32 KiB; ten live contexts total at most 320 KiB. Histories expire after 15 idle or 30 total minutes and are cleared on end, pause, off and shutdown. Cancelled/expired turns never enter retained history. Up to 512 identifier-only session tombstones remain for at most 30 minutes to reject ended contexts; up to 512 request IDs remain until their bounded deadlines to suppress replay. These contain no conversation text. Capacity exhaustion fails closed. Native HTTP bodies are capped at 16 KiB and the worker queue is bounded.

The relay can read guest messages. HTTPS/WSS is transport encryption, not end-to-end encryption. Application logs do not contain guest transcripts, tokens, raw IP addresses or private paths. The operator, local model service, process memory, crash-dump tools, hosting/CDN logs and browser/device software are separate observation/retention surfaces; this feature does not disable their existing policies. Use only non-sensitive input. A separate low-privilege OS account is additional isolation if desired; this in-process boundary is not an OS sandbox.

## Release, rollback and later voice

Deploy the tested single relay instance first, configure and check the connector second, and verify native guest readiness/cancellation/isolation third. Only then set Portfolio's public `enabled` flag and HTTPS relay origin. Portfolio currently publishes `main` through branch-based GitHub Pages; keep that method. Its optional built-output Actions workflow is separately gated. A newer disabled frontend remains useful while the backend is absent.

Rollback starts by disabling Portfolio configuration, stopping the connector and turning native guest mode off. Restarting the relay revokes all tokens and clears all retained relay conversations. Revert only the relevant committed feature changes using normal reviewable commits; never reset unrelated owner work or erase RuntimeData. Protocol version mismatches fail closed; release compatible repositories together.

Future voice work is separate: explicit browser playback from Revia's render-only Qwen path, then permissioned push-to-talk Whisper input, with format conversion, byte/duration limits, cancellation, no autoplay, no local speaker leakage and the same guest isolation. This release has no microphone or voice controls.
