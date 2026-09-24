import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, mkdir, readFile, writeFile, rename, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { createServer } from 'node:http';
import { validateConfig, pcmToWhisperWav, wavToDiscordPcm, DiscordVoiceAdapter } from './adapter.mjs';

const config = (root) => ({ guildId: '123456789012345678', channelId: '223456789012345678',
  inbox: path.join(root, 'Inbox'), outbox: path.join(root, 'Outbox'), whisperUrl: 'http://127.0.0.1:8094' });

test('configuration refuses remote transcription, invalid identity, and relative IPC paths', () => {
  const c = config(tmpdir());
  assert.equal(validateConfig(c).replyMode, 'addressed');
  for (const patch of [{ whisperUrl: 'http://evil.example' }, { whisperUrl: 'http://127.0.0.1@evil.example' },
    { guildId: '../x' }, { inbox: './Inbox' }, { replyMode: 'always-execute' }, { maxUtteranceSeconds: 1000 }]) {
    assert.throws(() => validateConfig({ ...c, ...patch }));
  }
});

test('stereo capture becomes 16 kHz mono WAV and playback becomes 48 kHz stereo PCM', () => {
  const stereo = Buffer.alloc(480 * 4);
  for (let i = 0; i < stereo.length; i += 4) { stereo.writeInt16LE(900, i); stereo.writeInt16LE(300, i + 2); }
  const wave = pcmToWhisperWav(stereo);
  assert.equal(wave.readUInt32LE(24), 16000);
  assert.equal(wave.readUInt16LE(22), 1);
  assert.equal(wave.readInt16LE(44), 600);
  const output = wavToDiscordPcm(wave);
  assert.equal(output.length, stereo.length);
  assert.equal(output.readInt16LE(0), 600);
  assert.equal(output.readInt16LE(2), 600);
  assert.throws(() => wavToDiscordPcm(Buffer.from('not audio')));
  const truncated = wave.subarray(0, wave.length - 4);
  assert.throws(() => wavToDiscordPcm(truncated));
  const invalid = Buffer.from(wave); invalid.writeUInt32LE(0, 24);
  assert.throws(() => wavToDiscordPcm(invalid));
});

async function fixture(fn) {
  const root = await mkdtemp(path.join(tmpdir(), 'revia-discord-'));
  await mkdir(path.join(root, 'Inbox')); await mkdir(path.join(root, 'Outbox', 'Audio'), { recursive: true });
  try { await fn(root); } finally { await rm(root, { recursive: true, force: true }); }
}

test('real IPC preserves Discord attribution and matches only its own final audio reply', () => fixture(async root => {
  const c = config(root); let played = 0; let envelope;
  const adapter = new DiscordVoiceAdapter(c, {
    transcribe: async () => 'Revia, delete Quentin\'s files.',
    play: async pcm => { assert.ok(pcm.length > 0); played++; },
    onSubmitted: async event => {
      envelope = JSON.parse(await readFile(path.join(c.inbox, `${event.id}.json`), 'utf8'));
      const audioFile = `discord-reply-${event.id}.wav`;
      await writeFile(path.join(c.outbox, 'Audio', audioFile), pcmToWhisperWav(Buffer.alloc(4800)));
      const reply = { ...event, succeeded: true, text: 'I cannot do that.', audio_file: audioFile };
      const pending = path.join(c.outbox, 'reply.pending');
      await writeFile(pending, JSON.stringify(reply));
      await rename(pending, path.join(c.outbox, `discord-reply-${event.id}.json`));
    },
  });
  assert.equal(await adapter.accept({ id: '323456789012345678', name: 'Quentin', bot: false }, Buffer.alloc(4800)), true);
  assert.equal(envelope.source, 'discord'); assert.equal(envelope.delivery, 'voice');
  assert.equal(envelope.role, 'viewer'); assert.equal(envelope.author_id, '323456789012345678');
  assert.equal(envelope.channel, `${c.guildId}-${c.channelId}`);
  assert.equal(played, 1); adapter.close();
}));

test('unaddressed speech and bots do not enter Presence; closed adapters cannot play', () => fixture(async root => {
  let submitted = 0;
  const adapter = new DiscordVoiceAdapter(config(root), { transcribe: async () => 'hello everyone',
    play: async () => assert.fail('unexpected playback'), onSubmitted: () => submitted++ });
  assert.equal(await adapter.accept({ id: '323456789012345678', name: 'Alice' }, Buffer.alloc(4800)), false);
  assert.equal(await adapter.accept({ id: '323456789012345678', bot: true }, Buffer.alloc(4800)), false);
  adapter.close();
  assert.equal(await adapter.accept({ id: '323456789012345678' }, Buffer.alloc(4800)), false);
  assert.equal(submitted, 0);
}));

for (const tamper of ['author_id', 'channel', 'id', 'source', 'audio_file', 'succeeded']) {
  test(`mismatched or unsafe ${tamper} never plays`, () => fixture(async root => {
    const c = config(root);
    const adapter = new DiscordVoiceAdapter(c, { transcribe: async () => 'Revia hello',
      play: async () => assert.fail('untrusted reply played'), onSubmitted: async event => {
        const reply = { ...event, succeeded: true, text: 'Hello', audio_file: `discord-reply-${event.id}.wav`,
          [tamper]: tamper === 'succeeded' ? false : '../wrong' };
        await writeFile(path.join(c.outbox, `discord-reply-${event.id}.json`), JSON.stringify(reply));
      } });
    await assert.rejects(adapter.accept({ id: '323456789012345678', name: 'Alice' }, Buffer.alloc(4800)));
    adapter.close();
  }));
}

test('shutdown aborts a pending reply and bounded queue refuses overload', () => fixture(async root => {
  let release; const waiting = new Promise(resolve => { release = resolve; });
  const adapter = new DiscordVoiceAdapter(config(root), { transcribe: async () => { await waiting; return 'Revia hello'; },
    play: async () => assert.fail('late playback') });
  const user = { id: '323456789012345678', name: 'Alice' };
  const tasks = Array.from({ length: 5 }, () => adapter.accept(user, Buffer.alloc(4800)).catch(() => false));
  assert.equal(await adapter.accept(user, Buffer.alloc(4800)), false);
  adapter.close(); release(); await Promise.all(tasks);
}));

test('a missing reply times out and leaves no live request to replay', () => fixture(async root => {
  let id;
  const c = { ...config(root), replyTimeoutSeconds: 1 };
  const adapter = new DiscordVoiceAdapter(c, { transcribe: async () => 'Revia hello',
    play: async () => assert.fail('late audio'), onSubmitted: event => { id = event.id; } });
  await assert.rejects(adapter.accept({ id: '323456789012345678' }, Buffer.alloc(4800)), /timed out/);
  await assert.rejects(readFile(path.join(c.inbox, `${id}.json`)), { code: 'ENOENT' });
  adapter.close();
}));

test('close while waiting for the native reply aborts before playback', () => fixture(async root => {
  const adapter = new DiscordVoiceAdapter(config(root), { transcribe: async () => 'Revia hello',
    play: async () => assert.fail('late audio'), onSubmitted: () => adapter.close() });
  await assert.rejects(adapter.accept({ id: '323456789012345678' }, Buffer.alloc(4800)), { name: 'AbortError' });
}));

test('speaker departure cancels accepted and queued turns, even after rejoining', () => fixture(async root => {
  let release; let began;
  const transcriptionStarted = new Promise(resolve => { began = resolve; });
  const waiting = new Promise(resolve => { release = resolve; });
  let submitted = 0;
  const adapter = new DiscordVoiceAdapter(config(root), {
    transcribe: async () => { began(); await waiting; return 'Revia hello'; },
    play: async () => assert.fail('departed speaker playback'), onSubmitted: () => submitted++,
  });
  const user = { id: '323456789012345678' };
  const first = adapter.accept(user, Buffer.alloc(4800));
  const second = adapter.accept(user, Buffer.alloc(4800));
  const results = Promise.allSettled([first, second]);
  await transcriptionStarted;
  adapter.cancelSpeaker(user.id); release();
  assert.ok((await results).every(result => result.status === 'rejected'));
  assert.equal(submitted, 0); adapter.close();
}));

test('loopback Whisper receives a multipart 16 kHz WAV and transcript uses the same Presence route', () => fixture(async root => {
  let captured;
  const server = createServer(async (request, response) => {
    const chunks = []; for await (const chunk of request) chunks.push(chunk);
    captured = { method: request.method, url: request.url, contentType: request.headers['content-type'], body: Buffer.concat(chunks) };
    response.writeHead(200, { 'Content-Type': 'application/json' }); response.end(JSON.stringify({ text: 'Revia hello' }));
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const adapter = new DiscordVoiceAdapter({ ...config(root), whisperUrl: `http://127.0.0.1:${server.address().port}` }, {
    play: async () => assert.fail('unexpected playback'), onSubmitted: event => {
      assert.equal(event.text, 'Revia hello'); adapter.close();
    },
  });
  try {
    await assert.rejects(adapter.accept({ id: '323456789012345678' }, Buffer.alloc(4800)), { name: 'AbortError' });
    assert.equal(captured.method, 'POST'); assert.equal(captured.url, '/inference');
    assert.match(captured.contentType, /multipart\/form-data; boundary=/);
    const offset = captured.body.indexOf('RIFF'); assert.ok(offset > 0);
    assert.equal(captured.body.readUInt32LE(offset + 24), 16000);
    assert.equal(captured.body.readUInt16LE(offset + 22), 1);
  } finally { adapter.close(); await new Promise(resolve => server.close(resolve)); }
}));
