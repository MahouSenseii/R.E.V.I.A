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

Stream simulation includes the stable platform ID and address decision a real connector
must derive from its SDK event:

```powershell
.\Tools\Presence\SendPresenceEvent.ps1 -Source stream -Channel live `
    -AuthorId viewer-42 -Author Viewer -AddressedToRevia `
    -Text "Revia, what do you think?"
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
  "author_id": "stable-platform-user-id",
  "author": "display name",
  "role": "viewer",
  "addressed_to_revia": true,
  "text": "message text"
}
```

`source` must be one of the configured `presence.allowedAdapters`. Text beginning with
`/` is rejected, and the session routes accepted events directly to conversation rather
than its command, goal, drawing, or application-action paths.

Stream events require `author_id`; display names are not identities. Viewer messages must
set `addressed_to_revia` unless that requirement is disabled. `broadcaster` and `moderator`
roles may pass the address and viewer cooldown gates, but they gain no action authority.
Duplicate source/id pairs, unsupported versions, control data, and repeated-character spam
are ignored or rejected before generation.

Public adapter turns have channel-scoped in-memory history. They do not receive the local
user's dialogue, compressed history, durable memories, desktop/camera context, or automatic
internet lookup. Stream speech is separately disabled by default with
`presence.speakStreamReplies`; enabling it routes selected replies through the existing
speech owner and does not grant the connector control over audio devices or OBS.

## Output contract

```json
{
  "version": 1,
  "id": "platform-message-id",
  "source": "discord",
  "channel": "general",
  "author_id": "stable-platform-user-id",
  "succeeded": true,
  "text": "Revia's reply",
  "reason": "",
  "timestamp": "2026-08-24T18:00:00Z"
}
```

The connector sends `text` back through its own platform SDK. Keep each connector
single-purpose: translate one platform's event into this schema and translate the reply
back. It should not contain Revia's model, memory, or permission logic.
