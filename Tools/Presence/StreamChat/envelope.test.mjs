import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, mkdir, readFile, writeFile, rename, rm, readdir } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { addressedToRevia, stripAddress, safeToken, validateConfig, PresenceEnvelope } from './envelope.mjs';

test('addressing recognises her name as a word, a mention, and a command', () => {
  assert.equal(addressedToRevia('Revia, what is a mutex?'), true);
  assert.equal(addressedToRevia('hey @revia do a flip'), true);
  assert.equal(addressedToRevia('!revia tell me a joke'), true);
  assert.equal(addressedToRevia('!ask what time is it'), true);
  assert.equal(addressedToRevia('previa is a car'), false);
  assert.equal(addressedToRevia('lol that boss'), false);
  assert.equal(stripAddress('Revia, what is a mutex?'), 'what is a mutex?');
  assert.equal(stripAddress('!revia tell me a joke'), 'tell me a joke');
  assert.equal(stripAddress('@Revia: hi'), 'hi');
  assert.equal(stripAddress('what does Revia think?'), 'what does Revia think?');
  assert.equal(safeToken('abc/../def 1'), 'abc-..-def-1');
  assert.equal(safeToken(''), 'missing-id');
});

test('configuration insists on absolute, distinct Presence paths and a known reply mode', () => {
  const good = { inbox: path.join(tmpdir(), 'Inbox'), outbox: path.join(tmpdir(), 'Outbox') };
  assert.equal(validateConfig(good).replyMode, 'addressed');
  assert.equal(validateConfig(good).name, 'Revia');
  for (const patch of [{ inbox: './Inbox' }, { outbox: good.inbox }, { replyMode: 'always' },
    { replyTimeoutSeconds: 1 }, { twitch: 'yes' }]) {
    assert.throws(() => validateConfig({ ...good, ...patch }));
  }
});

test('a message is written whole to the inbox and its reply is read once from the outbox', async () => {
  const root = await mkdtemp(path.join(tmpdir(), 'revia-stream-'));
  try {
    const config = validateConfig({ inbox: path.join(root, 'Inbox'), outbox: path.join(root, 'Outbox'),
      replyTimeoutSeconds: 5 });
    await mkdir(config.inbox); await mkdir(config.outbox);
    const envelope = new PresenceEnvelope(config);
    const event = await envelope.submit({ id: 'm 1', source: 'stream', channel: 'live', authorId: 'u1',
      author: 'Ann', role: 'viewer', text: 'what is a mutex?', addressed: true });
    assert.equal(event.id, 'm-1');
    const written = JSON.parse(await readFile(path.join(config.inbox, 'stream-m-1.json'), 'utf8'));
    assert.deepEqual(written, { version: 1, id: 'm-1', source: 'stream', channel: 'live', author_id: 'u1',
      author: 'Ann', role: 'viewer', addressed_to_revia: true, text: 'what is a mutex?' });
    assert.deepEqual((await readdir(config.inbox)).filter(name => name.endsWith('.pending')), []);

    const waiting = envelope.awaitReply('stream', 'm 1');
    const pending = path.join(config.outbox, 'reply.pending');
    await writeFile(pending, JSON.stringify({ version: 1, id: 'm-1', succeeded: true, text: 'A lock.' }));
    await rename(pending, path.join(config.outbox, 'stream-reply-m-1.json'));
    const reply = await waiting;
    assert.equal(reply.text, 'A lock.');
    assert.deepEqual(await readdir(config.outbox), []);

    const timedOut = new PresenceEnvelope({ ...config, replyTimeoutSeconds: 5 });
    timedOut.config.replyTimeoutSeconds = 0.3;
    assert.equal(await timedOut.awaitReply('stream', 'nobody'), null);
  } finally { await rm(root, { recursive: true, force: true }); }
});
