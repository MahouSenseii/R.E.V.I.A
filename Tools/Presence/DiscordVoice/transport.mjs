import { ChannelType, PermissionFlagsBits } from 'discord.js';
import { joinVoiceChannel, VoiceConnectionStatus } from '@discordjs/voice';

// The signal owns the connection from the instant it is created, including the
// interval before the caller's await resumes and can store the returned handle.
export async function openVoiceConnection({ client, config, signal, join = joinVoiceChannel }) {
  signal.throwIfAborted();
  const channel = await client.channels.fetch(config.channelId);
  signal.throwIfAborted();
  if (!channel || channel.type !== ChannelType.GuildVoice || channel.guildId !== config.guildId)
    throw new Error('Configure a regular voice channel in the selected server.');
  if (!channel.permissionsFor(client.user)?.has([PermissionFlagsBits.ViewChannel, PermissionFlagsBits.Connect, PermissionFlagsBits.Speak]))
    throw new Error('Bot requires View Channel, Connect, and Speak.');
  const connection = join({ guildId: config.guildId, channelId: config.channelId,
    adapterCreator: channel.guild.voiceAdapterCreator, selfDeaf: false, selfMute: false });
  const abort = () => { if (connection.state.status !== VoiceConnectionStatus.Destroyed) connection.destroy(); };
  signal.addEventListener('abort', abort, { once: true });
  connection.once(VoiceConnectionStatus.Destroyed, () => signal.removeEventListener('abort', abort));
  if (signal.aborted) { abort(); signal.throwIfAborted(); }
  return { connection, channel };
}
