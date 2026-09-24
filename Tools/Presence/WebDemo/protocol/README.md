# Version 1 wire reference

`index.js` is the canonical executable strict schema; `fixtures.json` contains literal cross-language examples. All objects reject unknown fields. `version` is exactly the integer 1. Epoch, session, request and browser idempotency identifiers are canonical lowercase random UUIDv4 strings. The browser cannot choose an epoch/session/request identifier, prompt, role, source, model, URL, path, or owner context. The relay generates identifiers cryptographically.

## Browser HTTP

Responses use JSON and `Cache-Control: no-store`, with exact configured origins and `Vary: Origin`. No bearer appears in a URL. The guest token is 32 random bytes encoded as base64url, returned once and stored by the relay as a SHA-256 digest. The client must keep it in memory. A bearer authorizes only its session. Foreign and unknown request IDs both return `404 {"error":"not_found"}`.

| Method/path | Body | Success |
|---|---|---|
| GET `/healthz` | None | 200 `{ok:true}`; relay process only |
| GET `/v1/status` | None | 200 `{state}` |
| POST `/v1/sessions` | `{inviteCode}` | 201 `{sessionId,token,expiresAt}`; expiresAt is Unix milliseconds |
| POST `/v1/messages` | `{text,idempotencyKey}` + bearer | 202 `{requestId,state,text?}` |
| GET `/v1/messages/:requestId` | Bearer | 200 `{requestId,state,text?}` |
| POST `/v1/messages/:requestId/cancel` | Bearer, no body | 200 `{requestId,state,text?}` |
| DELETE `/v1/session` | Bearer, no body | 204 |

Only invite admission is implemented; no anonymous production fallback or unverified challenge token exists. `text` is nonblank, at most 2,000 Unicode code points/8 KiB UTF-8 and must contain valid surrogate pairs. Entire JSON bodies are capped at 16 KiB before parsing. The same session/idempotency key and exact text returns the same request, including after completion. Different text returns 409. No replay is inferred from a timeout; the client retries the same key/body.

Availability states: `offline`, `starting`, `online`, `busy`, `paused`, `unavailable`. Request states: `queued`, `running`, `completed`, `cancelled`, `expired`, `failed`. Only completed results contain text; that text is literal output, never executable markup. Errors contain only `{error:public_code}`: `invalid_request`, `body_too_large`, `forbidden`, `admission_denied`, `unauthorized`, `not_found`, `pending_request`, `idempotency_conflict`, `rate_limited`, `capacity`, `unavailable`.

Cancellation immediately suppresses public publication, but the execution slot remains held until the native stop acknowledgement **and original native turn response** arrive. The UI can therefore see cancelled while subsequent work still queues. Host loss revokes all sessions. Pause/off/model-unavailable closes old sessions for new submissions (401), with terminal polling temporarily retained; start a fresh session after recovery.

## Outbound host WebSocket

Connect to `/v1/host` with `Authorization: Bearer HOST_TOKEN` and `X-Revia-Host: HOST_ID`. Browser Origin headers are forbidden on this channel; exactly one host is admitted. No query authentication, redirects, compression, binary frames or generic RPC. Maximum frame 32 KiB, 300 received frames/minute, maximum queued socket output 128 KiB. An invalid version, unknown field or stale epoch closes the connection.

Every frame includes `{version:1,type,epoch}`. The relay emits `welcome` with a newly generated epoch. The host must never invent/reuse an epoch or resend old turns after reconnect.

| Direction / type | Additional fields |
|---|---|
| Relay → host `welcome` | None |
| Host → relay `ready` | `state` reflecting native application readiness |
| Relay → host `turn` | `sessionId,requestId,text,deadline` (absolute Unix milliseconds) |
| Relay → host `cancel` | `sessionId,requestId` |
| Relay → host `end` | `sessionId` |
| Host → relay `result` | `sessionId,requestId,state,text?` (terminal only) |
| Host → relay `cancelled` | `sessionId,requestId` |

The connector checks readiness every three seconds; relay expires a missing readiness heartbeat after fifteen seconds by default. Busy state can admit a bounded waiting queue; dispatch requires online readiness. Results bind the exact active epoch/session/request and are ignored after cancellation/end/expiry. A completed result cannot acknowledge a cancel. Repeated results never dispatch another inference.

Deadline clocks must be synchronized. Connector/native allow at most five seconds of positive clock skew: a received deadline beyond local `now + 125000` is invalid. The connector forwards `min(receivedDeadline, localNow + 120000)` to native, which independently enforces the same local execution ceiling. Earlier deadlines are never extended. The relay's own 120-second total request lifetime remains unchanged.

## Narrow native API

Connector targets only the configured literal loopback origin, default `http://127.0.0.1:17864`, with separate `Authorization: Bearer REVIA_WEB_LOCAL_TOKEN`. It never follows redirects.

- GET `/web/v1/status` → `{state}`.
- POST `/web/v1/turn` with `{version:1,epoch,sessionId,requestId,text,deadline}` → `{requestId,state,text?}`.
- POST `/web/v1/cancel` with `{version:1,epoch,sessionId,requestId}` → `{requestId,state:"cancelled"}`. This acknowledges a stop request; the connector separately waits for the original turn to unwind.
- POST `/web/v1/end` with `{version:1,epoch,sessionId}` → `{state:"ended"}`.

The native owner control endpoint is intentionally absent from connector dispatch. Native non-2xx responses, invalid/oversized replies and diagnostic fields are collapsed to generic failed/unavailable public states. Native HTTP response bodies are capped at 32 KiB and text at 16 KiB. Cancellation/control requests have short timeouts; turn fetch deadlines derive only from the authenticated relay's bounded absolute deadline. Native must enforce its own deadline, no-overlap, session capacity/history limits and final filtering independently of this transport.

Native status `offline` specifically means owner-disabled public mode. The connector responds by stopping work and reconnect attempts; re-enabling requires starting a new connector process. `paused` is resumable without restarting the connector. Model failure and unreachable/invalid native responses use `unavailable`, so a temporary model problem is not mistaken for an owner instruction. Native status is checked before reconnect attempts as well as during the connected heartbeat.
