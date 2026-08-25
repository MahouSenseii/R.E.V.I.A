# Revia presence adapters

Revia's adapter boundary is deliberately local and conversation-only. A Discord bot,
stream-chat client, or game mod writes one JSON object to `RuntimeData/Presence/Inbox`.
Revia validates the source, size, rate, and command boundary, moves the input to
`Processed` or `Rejected`, and writes its reply to `RuntimeData/Presence/Outbox`.

This layer does not store service tokens and does not give an integration application or
shell authority. Network credentials and platform SDKs stay in the small connector that
owns them. Revia sees only the bounded text event.

## Send a live test event

```powershell
.\Tools\Presence\SendPresenceEvent.ps1 -Source discord -Channel general `
    -Author MahouSensei -Text "Hi Revia, are you there?"
```

Enable `Local Discord, stream, and game adapters` in the Presence tab first, then watch
the reply:

```powershell
.\Tools\Presence\WatchPresenceReplies.ps1 -Source discord
```

## Input contract

```json
{
  "version": 1,
  "id": "platform-message-id",
  "source": "discord",
  "channel": "general",
  "author": "display name",
  "text": "message text"
}
```

`source` must be one of the configured `presence.allowedAdapters`. Text beginning with
`/` is rejected, and the session routes accepted events directly to conversation rather
than its command, goal, drawing, or application-action paths.

## Output contract

```json
{
  "version": 1,
  "id": "platform-message-id",
  "source": "discord",
  "channel": "general",
  "succeeded": true,
  "text": "Revia's reply",
  "reason": "",
  "timestamp": "2026-08-24T18:00:00Z"
}
```

The connector sends `text` back through its own platform SDK. Keep each connector
single-purpose: translate one platform's event into this schema and translate the reply
back. It should not contain Revia's model, memory, or permission logic.
