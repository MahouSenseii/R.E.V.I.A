# Revia public text demo design

The approved brief is the user's Revia_Portfolio_Web_Demo_Implementation_Prompt.md. Implement in the existing R.E.V.I.A and Portfolio repositories on main; keep public activation disabled until configured. Starting revisions: Revia 5a36472, Portfolio bfbbc1c. No branch, reset, infrastructure purchase, or unrelated changes.

## Boundaries and ownership

Portfolio owns the accessible text UI and in-memory browser token. A single Node relay owns admission, ephemeral sessions, fair bounded dispatch, results and host epochs. An outbound Node connector translates only the versioned public chat protocol to an authenticated loopback native guest API. Revia owns a dedicated ephemeral guest lifecycle, a separate model router with a curated public profile, final response filtering, readiness, owner priority and request cancellation. Guest requests never enter Submit or the external Discord adapter queue.

The existing ReplyPublic path reads shared emotion causes/preferences/humanization and the external adapter loop records durable relationships. Web therefore uses an isolated overload of ReplyPublic with a dedicated router, a fixed public persona and no owner-state providers. The ordinary desktop runtime is not a guest dependency. This also prevents private profile system prompts from entering model requests. Existing Discord behavior remains covered by regression tests.

## Contract version 1

HTTP JSON: GET /healthz; GET /v1/status => {state}; POST /v1/sessions with {inviteCode} or {challengeToken} => {sessionId,token,expiresAt}; POST /v1/messages bearer auth with {text,idempotencyKey} => {requestId,state}; GET /v1/messages/:id => {requestId,state,text?}; POST /v1/messages/:id/cancel; DELETE /v1/session. Errors use {error: public_code}. States: queued, running, completed, cancelled, expired, failed. Browser availability: offline, starting, online, busy, paused, unavailable. Canonical strict schema and fixtures live under Tools/Presence/WebDemo/protocol.

Host WSS /v1/host uses Authorization bearer host credential and an exact configured host ID. Relay assigns an epoch. All dispatch/cancel/result frames bind epoch, sessionId and requestId; old epochs cannot publish. Host readiness is application readiness, not TCP connectivity. No retransmission of inference after reconnect. Guest session end propagates context deletion.

Native loopback API binds 127.0.0.1:17864 by default and requires separate REVIA_WEB_LOCAL_TOKEN (at least 32 bytes). GET /web/v1/status returns {state}; POST /web/v1/turn accepts {version:1,epoch,sessionId,requestId,text,deadline} (deadline epoch milliseconds) and returns {requestId,state,text?}; POST /web/v1/cancel accepts {version:1,epoch,sessionId,requestId}; POST /web/v1/end accepts {version:1,epoch,sessionId}; POST /web/v1/control is owner-only local control {enabled,paused}. The connector never forwards arbitrary routes or bodies. Cancellation acknowledges native stop request and suppresses late history/result publication; context deletion waits for no network operations. Tokens and identifiers never become filenames.

## Limits and threats

Messages: 2000 Unicode code points, 8192 UTF-8 bytes; bounded JSON bodies and WebSocket frames. Ten sessions, 15-minute idle/30-minute total expiry. One pending turn per session; one running turn globally and four waiting. Six submissions/minute/session plus network-source and global daily bounds. At most 120 seconds from admission, including queueing. Bound histories, results, idempotency entries, rate keys and aggregate retained bytes. Exact origin matching with no-store/Vary; CORS is additional browser protection, never authorization. Tokens are random, independently issued and only hashes are retained by the relay. Invite-only admission is the supported first public mode; no public development bypass.

Threat tests cover forged roles/unknown fields, cross-guest reads/cancellation, stale epochs/results, queue flooding, reconnect, owner preemption, expiration, malicious literal HTML, private-state canaries in actual model requests, and cancelled history. Guest model requests have no tools, memory access, private prompts, screenshots or dynamic owner state. No guest transcript logging or file IPC; hosting access logs are separately documented. TLS terminates at Render; outbound WSS validates certificates. Secrets stay in environment storage. A low-privilege OS account remains an optional defense, not a claim of sandboxing within the desktop process.

## Release and verification

Run native canary/lifecycle tests and full CTest, relay/connector protocol and transport tests, Discord tests, Portfolio tests/build, built-output browser checks, and browser-to-native mock-model integration. Attempt a real local-model smoke test only using isolated guest state. Record exact commands/exit codes and limits in evidence documentation. Commit and push coherent verified changes on main in each repository. Render deployment needs an owner account and secrets; prepare Docker/Render artifacts without claiming deployment. Portfolio configuration stays disabled and works under /Portfolio/ or a custom-domain root. Voice is documentation-only future work.
