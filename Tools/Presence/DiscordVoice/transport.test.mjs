import test from 'node:test';
import assert from 'node:assert/strict';
import { Readable } from 'node:stream';
import { createAudioResource, StreamType, generateDependencyReport } from '@discordjs/voice';
import prism from 'prism-media';
import { pcmToWhisperWav, wavToDiscordPcm } from './adapter.mjs';
import { openVoiceConnection } from './transport.mjs';
import { EventEmitter } from 'node:events';
import { ChannelType } from 'discord.js';

test('shutdown during channel fetch cannot create a late voice connection', async () => {
  let resolve; const fetch = new Promise(done => { resolve = done; });
  const controller = new AbortController(); let joins = 0;
  const pending = openVoiceConnection({ client: { channels: { fetch: () => fetch } },
    config: { channelId: 'channel', guildId: 'guild' }, signal: controller.signal,
    join: () => { joins++; } });
  controller.abort(); resolve({});
  await assert.rejects(pending, { name: 'AbortError' });
  assert.equal(joins, 0);
});

test('newly created connection is owned by shutdown before caller resumes', async () => {
  const controller = new AbortController();
  const connection = new EventEmitter(); connection.state = { status: 'connecting' };
  connection.destroy = () => { connection.state.status = 'destroyed'; connection.emit('destroyed'); };
  const channel = { type: ChannelType.GuildVoice, guildId: 'guild',
    guild: { voiceAdapterCreator: {} }, permissionsFor: () => ({ has: () => true }) };
  await openVoiceConnection({ client: { user: {}, channels: { fetch: async () => channel } },
    config: { channelId: 'channel', guildId: 'guild' }, signal: controller.signal, join: () => connection });
  controller.abort(); assert.equal(connection.state.status, 'destroyed');
});

test('installed Discord transport encodes and decodes synthetic audio without a connection', async () => {
  const report = generateDependencyReport();
  assert.match(report, /@snazzah\/davey: 0\./);
  assert.match(report, /@noble\/ciphers: 2\./);
  const pcm = Buffer.alloc(960 * 4 * 10);
  for (let i = 0; i < pcm.length / 4; i++) {
    const value = Math.round(3000 * Math.sin(i * 2 * Math.PI * 440 / 48000));
    pcm.writeInt16LE(value, i * 4); pcm.writeInt16LE(value, i * 4 + 2);
  }
  const resource = createAudioResource(Readable.from([wavToDiscordPcm(pcmToWhisperWav(pcm))]), { inputType: StreamType.Raw });
  const decoder = new prism.opus.Decoder({ rate: 48000, channels: 2, frameSize: 960 });
  const decoded = [];
  resource.playStream.pipe(decoder);
  for await (const chunk of decoder) decoded.push(chunk);
  const output = Buffer.concat(decoded);
  assert.equal(output.length, pcm.length);
  assert.ok(output.some(byte => byte !== 0));
});
