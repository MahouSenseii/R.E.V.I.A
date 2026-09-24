import { readFile, stat } from 'node:fs/promises';
import { Readable } from 'node:stream';
import { Client, GatewayIntentBits, Events } from 'discord.js';
import { entersState, VoiceConnectionStatus, createAudioPlayer,
  createAudioResource, AudioPlayerStatus, StreamType, EndBehaviorType, NoSubscriberBehavior,
  generateDependencyReport } from '@discordjs/voice';
import prism from 'prism-media';
import { validateConfig, DiscordVoiceAdapter } from './adapter.mjs';
import { openVoiceConnection } from './transport.mjs';

const config = validateConfig(JSON.parse(await readFile(process.argv[2] ?? new URL('./config.json', import.meta.url), 'utf8')));
for (const directory of [config.inbox, config.outbox]) {
  if (!(await stat(directory)).isDirectory()) throw new Error('Start Revia first and configure its actual Presence paths.');
}
if (process.argv.includes('--check')) {
  console.log('Local configuration and Presence directories are valid. No Discord connection opened.');
  console.log(generateDependencyReport());
} else {
  if (!process.env.DISCORD_BOT_TOKEN) throw new Error('Set DISCORD_BOT_TOKEN in the local environment.');
  const client = new Client({ intents: [GatewayIntentBits.Guilds, GatewayIntentBits.GuildVoiceStates] });
  const player = createAudioPlayer({ behaviors: { noSubscriber: NoSubscriberBehavior.Stop } });
  const captures = new Map();
  const shutdown = new AbortController();
  let connection; let adapter; let stopping = false;

  function stop(reason) {
    if (stopping) return;
    stopping = true; shutdown.abort(); adapter?.close(); player.stop(true);
    for (const capture of captures.values()) capture.cancel();
    captures.clear();
    if (connection && connection.state.status !== VoiceConnectionStatus.Destroyed) connection.destroy();
    client.destroy();
    console.log(reason);
  }
  process.once('SIGINT', () => stop('Discord voice stopped.'));
  process.once('SIGTERM', () => stop('Discord voice stopped.'));
  client.on(Events.Error, () => { process.exitCode = 1; stop('Discord client failed; restart the connector after checking connectivity.'); });
  player.on('error', () => { process.exitCode = 1; stop('Discord playback failed; restart the connector.'); });
  client.on(Events.VoiceStateUpdate, (_old, state) => {
    if (state.guild.id !== config.guildId) return;
    if (state.id === client.user?.id && state.channelId !== config.channelId)
      stop('Revia left or was moved from the configured channel.');
    const capture = captures.get(state.id);
    if (capture && state.channelId !== config.channelId) capture.cancel();
    if (state.channelId !== config.channelId) adapter?.cancelSpeaker(state.id);
  });

  client.once(Events.ClientReady, async () => {
    try {
      const opened = await openVoiceConnection({ client, config, signal: shutdown.signal });
      connection = opened.connection;
      const channel = opened.channel;
      shutdown.signal.throwIfAborted();
      connection.on('error', () => { process.exitCode = 1; stop('Discord voice connection failed.'); });
      // Fail closed on disconnect. Do not replay a delayed reply into a later connection.
      connection.on(VoiceConnectionStatus.Disconnected, () => stop('Discord voice disconnected; restart to rejoin.'));
      await entersState(connection, VoiceConnectionStatus.Ready,
        AbortSignal.any([shutdown.signal, AbortSignal.timeout(30000)]));
      if (stopping) return;
      connection.subscribe(player);
      adapter = new DiscordVoiceAdapter(config, { play: async (pcm, signal) => {
        signal.throwIfAborted();
        if (connection.state.status !== VoiceConnectionStatus.Ready || channel.guild.members.me?.voice.channelId !== config.channelId)
          throw new Error('Voice channel is no longer connected.');
        const resource = createAudioResource(Readable.from([pcm]), { inputType: StreamType.Raw });
        for (const capture of captures.values()) capture.cancel();
        const abort = () => player.stop(true);
        signal.addEventListener('abort', abort, { once: true });
        try {
          player.play(resource);
          await entersState(player, AudioPlayerStatus.Playing, AbortSignal.any([signal, AbortSignal.timeout(10000)]));
          await entersState(player, AudioPlayerStatus.Idle, AbortSignal.any([signal, AbortSignal.timeout(130000)]));
          signal.throwIfAborted();
        } finally { signal.removeEventListener('abort', abort); player.stop(true); }
      } });
      connection.receiver.speaking.on('start', userId => {
        const member = channel.members.get(userId);
        if (stopping || !member || member.user.bot || captures.has(userId) || captures.size >= 4 ||
            player.state.status !== AudioPlayerStatus.Idle) return;
        const source = connection.receiver.subscribe(userId, { end: { behavior: EndBehaviorType.AfterSilence, duration: 900 } });
        const decoder = new prism.opus.Decoder({ rate: 48000, channels: 2, frameSize: 960 });
        const chunks = []; let bytes = 0; let finished = false;
        const maximum = config.maxUtteranceSeconds * 48000 * 4;
        const finish = (submit) => {
          if (finished) return; finished = true; clearTimeout(timer);
          source.unpipe(decoder); source.destroy(); decoder.destroy(); captures.delete(userId);
          if (submit && !stopping && bytes >= 1920) {
            adapter.accept({ id: userId, name: member.displayName, bot: member.user.bot }, Buffer.concat(chunks, bytes))
              .catch(() => { if (!stopping) console.error('Voice turn failed; check Revia Presence, Whisper, and Qwen status.'); });
          }
        };
        const timer = setTimeout(() => finish(true), config.maxUtteranceSeconds * 1000);
        captures.set(userId, { cancel: () => finish(false) });
        decoder.on('data', chunk => {
          const keep = Math.min(chunk.length, maximum - bytes);
          if (keep > 0) { chunks.push(chunk.subarray(0, keep)); bytes += keep; }
          if (bytes >= maximum) finish(true);
        });
        source.on('error', () => finish(false)); decoder.on('error', () => finish(false));
        decoder.on('end', () => finish(true)); source.pipe(decoder);
      });
      console.log(`Revia joined the configured voice channel. Reply mode: ${config.replyMode}. Ctrl+C leaves.`);
    } catch (error) {
      console.error('Could not start Discord voice. Check the channel, bot permissions, and installed voice dependencies.');
      process.exitCode = 1; stop('Discord voice stopped.');
    }
  });
  try { await client.login(process.env.DISCORD_BOT_TOKEN); }
  catch { process.exitCode = 1; stop('Discord login failed. Check the local bot token.'); }
}
