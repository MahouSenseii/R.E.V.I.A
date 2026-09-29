# Revia stream connectors: Twitch, YouTube, OBS

One small Node program (Node 22 or newer, no npm packages) that puts a running Revia in
front of a live chat and keeps OBS in step with her. It speaks to Revia only through her
Presence inbox and outbox, exactly like the Discord connector: every chat line becomes
one JSON envelope, and the only thing that goes back to the platform is the reply she
wrote to the outbox after her own filters. Tokens live in the environment, never in a file.

What Revia does with the messages is decided on her side: the chat selector picks who
she answers (owner and moderators first, whoever addresses her before overheard chat, a
supporter always acknowledged), the stream safety filter says `Filtered.` in place of a
sentence that must not go out, and `/stream kill` holds everything.

## Set up

```powershell
cd .\Tools\Presence\StreamChat
Copy-Item config.example.json config.json
notepad config.json
node main.mjs config.json --check
```

`inbox` and `outbox` are the **absolute** Presence paths of the Revia you are running
(for example `C:/Users/you/Documents/R.E.V.I.A/build/debug/RuntimeData/Presence/Inbox`).
In Revia's **Presence** tab turn on **Enable local Discord, stream, and game adapters**.
`replyMode: "addressed"` means she is offered a chat line as addressed to her when it
says her name, `@revia`, `!revia` or `!ask`; `"conversation"` marks every line as
addressed and leaves the choice to her selector and talkativeness.

### Twitch

Reading needs nothing: leave `TWITCH_OAUTH_TOKEN` unset and the connector joins as an
anonymous read-only nick. To let her reply in chat, make a user access token for the
bot account with the `chat:read` and `chat:edit` scopes (the [Twitch token
generator](https://twitchtokengenerator.com/) or your own app), set `login` to that
account's login name, and:

```powershell
$env:TWITCH_OAUTH_TOKEN = 'oauth:....'
node main.mjs config.json
```

Subs, resubs, gifts and raids arrive as supporter events she always acknowledges. Replies
are paced under Twitch's 20 messages per 30 seconds.

### YouTube

Set `videoId` to the live stream's video id (or `liveChatId` directly). Reading needs an
API key (`YOUTUBE_API_KEY`) from a Google Cloud project with the YouTube Data API v3
enabled; each poll costs one quota unit and the API sets the polling interval. Replying
needs an OAuth user token (`YOUTUBE_OAUTH_TOKEN`) for the channel and costs 50 units a
message; without it she reads and the reply is logged, not sent. Super chats and new
members arrive as supporter events.

### OBS

Enable the WebSocket server in OBS (Tools, WebSocket Server Settings; OBS 28 or newer)
and set `OBS_WEBSOCKET_PASSWORD` if you gave it one. `brbScene` is the scene the
connector cuts to while Revia's avatar state reads `brb` (that is `/stream kill`), and
`liveScene` is where it returns on `/stream resume`. `captionSource` names a text source
that shows the latest reply she gave (the filtered one), read from
`RuntimeData/Presence/caption.txt`.

## Test it without any platform

```powershell
npm test
```

The tests run a fake Twitch on a local socket, a fake YouTube behind `fetch`, and a fake
OBS behind the WebSocket interface. Nothing connects out.

## What the connector never does

It never runs a command, never touches Revia's memory or settings, and never sends a
line she did not write to the outbox. A message she passed over (the selector chose
someone else, or the filter held it) gets no reply, and the log says why.
