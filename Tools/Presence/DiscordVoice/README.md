# Discord voice for Revia

This connector joins one configured Discord server voice channel. It decodes each
human speaker's audio, transcribes it with Revia's local whisper.cpp server, and
submits a public conversation event to the running Revia session. Revia produces
the final reply and renders it with the active profile's assigned Qwen voice. The
connector resamples that WAV to 48 kHz stereo and sends Opus audio to Discord.

There is no second model, persona, or memory store in the connector. Revia keeps
her existing identity, affect, relationships, and channel conversation history.
The existing public-audience boundary deliberately excludes private local-user
history, durable private memories, desktop/camera context, and credentials.
Discord speech cannot call the local command, goal, shell, or desktop-action
routes, including when the speaker uses the local user's display name.

## Prerequisites

- Node.js 22.12 or newer. Dependencies are pinned in `package-lock.json`.
- A Discord **bot account**, invited to the target server with **View Channel**,
  **Connect**, and **Speak** permissions in a regular voice channel. Administrator
  permission, user-account tokens, and the privileged Message Content intent are
  not needed. Stage channels and direct calls are not supported.
- Revia running with **Local Discord, stream, and game adapters** enabled in the
  Presence tab and `discord` in `presence.allowedAdapters`.
- Speech enabled and a Qwen voice assigned to Revia's active profile. Audio render
  failure is reported in Revia's adapter status; there is no Windows SAPI fallback.
- Revia's persistent whisper.cpp service running at its configured loopback host
  and port (default `http://127.0.0.1:8094`). The connector does not launch or
  reconfigure that service. Select the server recognition backend in Revia and
  check that recognition is ready before starting the connector.

The pinned `@discordjs/voice` 0.19.2 includes the DAVE library used for Discord's
end-to-end encrypted voice transport. `@noble/ciphers` supplies XChaCha20-Poly1305
and `opusscript` supplies Opus; FFmpeg is not required. Discord does not officially
document audio receive, so library support can change with Discord's protocol.
See the [voice library documentation](https://github.com/discordjs/discord.js/tree/main/packages/voice)
and [Discord voice protocol](https://github.com/discord/discord-api-docs/blob/main/developers/topics/voice-connections.mdx).

## Local setup

From this directory:

```powershell
npm ci --ignore-scripts
Copy-Item config.example.json config.json
```

Edit `config.json`:

- Copy the server and voice-channel IDs from Discord's Developer Mode.
- Set `inbox` and `outbox` to the **actual absolute Presence paths for the Revia
  executable you are running**. A source checkout and a packaged build may have
  different runtime roots. Custom `presence.inboxPath`/`outboxPath` settings must
  match. Start Revia first; it creates these directories.
- Set `whisperUrl` to Revia's loopback recognition server origin. Remote hosts and
  redirects are rejected so captured audio cannot silently go to another service.
- Keep `replyMode: "addressed"` to answer only transcripts containing the name
  **Revia**. Set `"conversation"` to accept other human speech in this channel.
  Recognition must transcribe the name correctly for addressed mode to trigger.

Validate locally, without logging into Discord or opening a microphone:

```powershell
node main.mjs config.json --check
npm test
```

Provide the bot token through the `DISCORD_BOT_TOKEN` environment variable. Do not
put it in `config.json`, source files, transcripts, or chat. For an interactive
PowerShell session, this avoids putting the token in command history:

```powershell
$discordCredential = Read-Host 'Discord bot token' -AsSecureString
$env:DISCORD_BOT_TOKEN = [System.Net.NetworkCredential]::new('', $discordCredential).Password
try { node main.mjs config.json }
finally { Remove-Item Env:DISCORD_BOT_TOKEN -ErrorAction SilentlyContinue }
```

Starting that command is the explicit join action. **Ctrl+C** leaves the channel
and cancels pending work. A disconnect, kick, or move to another channel stops the
connector; restart it explicitly to rejoin. It does not join on Revia startup.

## Behavior and limits

- Discord user IDs supply speaker attribution; display names never grant authority.
- At most four simultaneous speakers are decoded. A recording ends after 900 ms
  of silence or `maxUtteranceSeconds` (default 20, maximum 30).
- The connector holds at most one active turn and four waiting recordings. Excess
  input is dropped. Revia also applies its own Presence rate and queue limits.
- Playback is half duplex: new captures are ignored while Revia speaks, and
  unfinished captures are cancelled before playback. Discord barge-in is not
  implemented in this version.
- Reply matching checks the random request ID, source, server/channel, and user
  ID. A speaker leaving or moving cancels that speaker's accepted turns, including
  queued work and current playback. Old replies are never replayed after restart. Missing replies time out after
  180 seconds by default, including rate-limited or rejected Presence events.
- Raw incoming audio stays in bounded memory and is not saved. Public transcript
  envelopes follow Revia's existing Processed/Rejected retention settings.
- Revia writes reply WAVs atomically under `Outbox/Audio`, capped at 24 MiB per
  artifact. The connector deletes its own request/reply/audio artifacts after use.
  Revia sweeps leftover WAVs every 10 seconds, retaining at most eight and expiring
  them after ten minutes while the Presence adapter worker is running.
- Audio uses the active profile's Qwen reference and existing speech normalization
  and length limit. Local Windows playback is bypassed. Inline nonverbal clip-bank
  sounds and local microphone barge-in are not part of this transport.

## Extended Presence contract

The connector writes the usual version-1 envelope with `source: "discord"`,
`delivery: "voice"`, a stable `author_id`, and a server/channel-scoped `channel`.
Omitting `delivery` preserves the existing text behavior. Other sources cannot
request voice delivery through this extension.

Revia's reply adds `delivery`, `audio_file`, and `audio_error`. `audio_file` is a
basename under the outbox's `Audio` directory, never an arbitrary path. Audio is
published before its JSON reply. A successful text conversation can still have
an audio error; the connector reports failure and plays nothing in that case.
Only the final, privacy-filtered public reply is synthesized.

## Verification scope

Automated tests cover synthetic PCM/WAV conversion, actual local Opus encoding
and decoding, bounded queues, cancellation, timeout, identity/channel matching,
and disposable Presence file exchange. Native tests cover voice envelope parsing,
audio publication, missing-voice failure, and the public session route for a
spoken desktop-control request. A live Discord join, real incoming speech, and
Qwen GPU synthesis still require a configured bot/channel and local models.
