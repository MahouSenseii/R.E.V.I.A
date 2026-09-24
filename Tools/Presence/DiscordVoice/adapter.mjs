import { randomUUID } from 'node:crypto';
import { open, writeFile, rename, unlink } from 'node:fs/promises';
import path from 'node:path';
import { setTimeout as delay } from 'node:timers/promises';

const snowflake = /^\d{17,20}$/;
const maxAudioBytes = 24 * 1024 * 1024;

export function validateConfig(input) {
  const c = { replyMode: 'addressed', maxUtteranceSeconds: 20, replyTimeoutSeconds: 180,
    language: 'en', ...input };
  if (!snowflake.test(c.guildId) || !snowflake.test(c.channelId)) throw new Error('Configure guildId and channelId as Discord IDs.');
  if (!path.isAbsolute(c.inbox ?? '') || !path.isAbsolute(c.outbox ?? '') || c.inbox === c.outbox)
    throw new Error('Configure distinct absolute Presence inbox and outbox paths.');
  const url = new URL(c.whisperUrl);
  if (url.protocol !== 'http:' || !['127.0.0.1', '[::1]'].includes(url.hostname) ||
      url.username || url.password || url.search || url.hash || url.pathname !== '/')
    throw new Error('Whisper must use a loopback HTTP server origin.');
  if (!['addressed', 'conversation'].includes(c.replyMode)) throw new Error('Unknown replyMode.');
  if (!Number.isInteger(c.maxUtteranceSeconds) || c.maxUtteranceSeconds < 1 || c.maxUtteranceSeconds > 30)
    throw new Error('maxUtteranceSeconds must be 1–30.');
  if (!Number.isInteger(c.replyTimeoutSeconds) || c.replyTimeoutSeconds < 1 || c.replyTimeoutSeconds > 300)
    throw new Error('replyTimeoutSeconds must be 1–300.');
  if (typeof c.language !== 'string' || !/^[a-z]{2,3}$/.test(c.language)) throw new Error('Invalid Whisper language.');
  return Object.freeze(c);
}

// Discord receive PCM is signed 16-bit stereo, 48 kHz. Average channels and each
// group of three frames for Whisper's 16 kHz mono input; no recording is persisted.
export function pcmToWhisperWav(pcm) {
  if (!Buffer.isBuffer(pcm) || pcm.length > 30 * 48000 * 4) throw new Error('Capture exceeds the audio bound.');
  const samples = Math.floor(pcm.length / 12);
  const wav = Buffer.alloc(44 + samples * 2);
  wav.write('RIFF'); wav.writeUInt32LE(wav.length - 8, 4); wav.write('WAVEfmt ', 8);
  wav.writeUInt32LE(16, 16); wav.writeUInt16LE(1, 20); wav.writeUInt16LE(1, 22);
  wav.writeUInt32LE(16000, 24); wav.writeUInt32LE(32000, 28); wav.writeUInt16LE(2, 32);
  wav.writeUInt16LE(16, 34); wav.write('data', 36); wav.writeUInt32LE(samples * 2, 40);
  for (let i = 0; i < samples; i++) {
    let total = 0;
    for (let j = 0; j < 6; j++) total += pcm.readInt16LE(i * 12 + j * 2);
    wav.writeInt16LE(Math.round(total / 6), 44 + i * 2);
  }
  return wav;
}

// Qwen returns PCM16 RIFF/WAV at its own sample rate. Parse chunks rather than
// assuming a 44-byte header (libsndfile may include metadata), then resample.
export function wavToDiscordPcm(wav) {
  if (!Buffer.isBuffer(wav) || wav.length < 44 || wav.length > maxAudioBytes ||
      wav.toString('ascii', 0, 4) !== 'RIFF' || wav.toString('ascii', 8, 12) !== 'WAVE' ||
      wav.readUInt32LE(4) + 8 !== wav.length) throw new Error('Invalid or oversized voice WAV.');
  let format; let data;
  for (let offset = 12; offset + 8 <= wav.length;) {
    const kind = wav.toString('ascii', offset, offset + 4);
    const size = wav.readUInt32LE(offset + 4); offset += 8;
    if (offset + size > wav.length) throw new Error('Truncated WAV chunk.');
    if (kind === 'fmt ') {
      if (size < 16) throw new Error('Invalid WAV format.');
      format = { codec: wav.readUInt16LE(offset), channels: wav.readUInt16LE(offset + 2),
        rate: wav.readUInt32LE(offset + 4), align: wav.readUInt16LE(offset + 12), bits: wav.readUInt16LE(offset + 14) };
    }
    if (kind === 'data') data = wav.subarray(offset, offset + size);
    offset += size + (size % 2);
  }
  if (!format || !data || format.codec !== 1 || ![1, 2].includes(format.channels) ||
      format.bits !== 16 || format.align !== format.channels * 2 || format.rate < 8000 || format.rate > 96000 ||
      data.length % format.align !== 0) throw new Error('Unsupported WAV format; PCM16 mono/stereo required.');
  const frames = data.length / format.align;
  const outputFrames = Math.floor(frames * 48000 / format.rate);
  if (!frames || outputFrames > 48000 * 120) throw new Error('Voice reply is empty or longer than two minutes.');
  const output = Buffer.alloc(outputFrames * 4);
  for (let i = 0; i < outputFrames; i++) {
    const position = i * format.rate / 48000;
    const left = Math.min(frames - 1, Math.floor(position)); const right = Math.min(frames - 1, left + 1);
    for (let channel = 0; channel < 2; channel++) {
      const index = Math.min(channel, format.channels - 1) * 2;
      const a = data.readInt16LE(left * format.align + index); const b = data.readInt16LE(right * format.align + index);
      output.writeInt16LE(Math.round(a + (b - a) * (position - left)), i * 4 + channel * 2);
    }
  }
  return output;
}

async function readBounded(file, limit) {
  const handle = await open(file, 'r');
  try {
    const stat = await handle.stat();
    if (!stat.isFile() || stat.size > limit) throw new Error('Adapter artifact exceeds its size bound.');
    const buffer = Buffer.alloc(Math.min(stat.size + 1, limit + 1));
    const { bytesRead } = await handle.read(buffer, 0, buffer.length, 0);
    if (bytesRead !== stat.size) throw new Error('Adapter artifact changed while reading.');
    return buffer.subarray(0, bytesRead);
  } finally { await handle.close(); }
}

async function readResponseBounded(response, limit) {
  const parts = []; let size = 0;
  for await (const part of response.body) {
    size += part.length;
    if (size > limit) throw new Error('Whisper response exceeds its size bound.');
    parts.push(part);
  }
  return Buffer.concat(parts).toString('utf8');
}

export class DiscordVoiceAdapter {
  #tail = Promise.resolve(); #pending = 0; #controller = new AbortController(); #turns = new Map();
  constructor(config, { play, transcribe, onSubmitted = () => {} }) {
    this.config = validateConfig(config);
    this.play = play; this.transcribe = transcribe ?? this.#transcribe.bind(this); this.onSubmitted = onSubmitted;
  }
  get closed() { return this.#controller.signal.aborted; }
  close() { this.#controller.abort(); }
  cancelSpeaker(id) {
    for (const controller of this.#turns.get(id) ?? []) controller.abort();
  }

  // One turn at a time; at most four queued captures plus the current turn.
  async accept(speaker, pcm) {
    if (this.closed || speaker.bot || !snowflake.test(speaker.id) || this.#pending >= 5 ||
        !Buffer.isBuffer(pcm) || pcm.length < 1920 || pcm.length > this.config.maxUtteranceSeconds * 48000 * 4) return false;
    this.#pending++;
    const controller = new AbortController();
    if (!this.#turns.has(speaker.id)) this.#turns.set(speaker.id, new Set());
    this.#turns.get(speaker.id).add(controller);
    const signal = AbortSignal.any([this.#controller.signal, controller.signal]);
    const task = this.#tail.then(() => this.#turn(speaker, pcm, signal));
    this.#tail = task.catch(() => {}).finally(() => {
      this.#pending--;
      const turns = this.#turns.get(speaker.id); turns.delete(controller);
      if (!turns.size) this.#turns.delete(speaker.id);
    });
    return task;
  }

  async #transcribe(wav, signal) {
    const form = new FormData(); form.append('file', new Blob([wav], { type: 'audio/wav' }), 'discord.wav');
    form.append('response_format', 'json'); form.append('language', this.config.language);
    const response = await fetch(new URL('/inference', this.config.whisperUrl), {
      method: 'POST', body: form, redirect: 'error', signal: AbortSignal.any([signal, AbortSignal.timeout(60000)]),
    });
    if (!response.ok) throw new Error(`Whisper returned HTTP ${response.status}.`);
    const result = JSON.parse(await readResponseBounded(response, 64 * 1024));
    if (typeof result.text !== 'string') throw new Error('Whisper returned no transcript.');
    return result.text;
  }

  async #turn(speaker, pcm, signal) {
    signal.throwIfAborted();
    const text = (await this.transcribe(pcmToWhisperWav(pcm), signal)).trim();
    signal.throwIfAborted();
    const addressed = /\brevia\b/i.test(text);
    if (!text || Buffer.byteLength(text) > 4000 || /^[\/]/.test(text) ||
        (this.config.replyMode === 'addressed' && !addressed)) return false;
    const id = `voice-${randomUUID()}`;
    const event = { version: 1, id, source: 'discord', delivery: 'voice',
      channel: `${this.config.guildId}-${this.config.channelId}`, author_id: speaker.id,
      author: String(speaker.name ?? speaker.id).replace(/[\x00-\x1f\x7f]/g, '').slice(0, 80) || speaker.id,
      role: 'viewer', addressed_to_revia: addressed, text };
    const pendingFile = path.join(this.config.inbox, `${id}.pending`);
    const inputFile = path.join(this.config.inbox, `${id}.json`);
    const replyFile = path.join(this.config.outbox, `discord-reply-${id}.json`);
    const audioName = `discord-reply-${id}.wav`;
    const audioFile = path.join(this.config.outbox, 'Audio', audioName);
    try {
      await writeFile(pendingFile, JSON.stringify(event), { flag: 'wx' });
      signal.throwIfAborted(); await rename(pendingFile, inputFile); await this.onSubmitted(event);
      const deadline = Date.now() + this.config.replyTimeoutSeconds * 1000;
      let reply;
      while (Date.now() < deadline) {
        signal.throwIfAborted();
        try { reply = JSON.parse((await readBounded(replyFile, 64 * 1024)).toString('utf8')); break; }
        catch (error) { if (error.code !== 'ENOENT') throw error; }
        await delay(150, undefined, { signal });
      }
      if (!reply) throw new Error('Revia reply timed out; check Presence and speech status.');
      if (reply.version !== 1 || reply.id !== id || reply.source !== 'discord' || reply.channel !== event.channel ||
          reply.author_id !== event.author_id || reply.succeeded !== true || reply.audio_file !== audioName)
        throw new Error('Revia returned no matching voice reply; check the local adapter status.');
      const audio = wavToDiscordPcm(await readBounded(audioFile, maxAudioBytes));
      signal.throwIfAborted(); await this.play(audio, signal);
      return true;
    } finally {
      // Only this request's exact artifacts. Late native replies are expired by Revia.
      await Promise.all([pendingFile, inputFile, replyFile, audioFile].map(file => unlink(file).catch(() => {})));
    }
  }
}
