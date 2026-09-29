import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, writeFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { createHash } from 'node:crypto';
import { authenticationResponse, identifyMessage, requestMessage, ObsClient } from './obs.mjs';

test('the authentication answer is what obs-websocket documents', () => {
  const password = 'hunter2', salt = 'c2FsdA==', challenge = 'Y2hhbGxlbmdl';
  const secret = createHash('sha256').update(password + salt).digest('base64');
  const expected = createHash('sha256').update(secret + challenge).digest('base64');
  assert.equal(authenticationResponse(password, salt, challenge), expected);
  const identify = identifyMessage({ op: 0, d: { rpcVersion: 1, authentication: { salt, challenge } } }, password);
  assert.equal(identify.op, 1);
  assert.equal(identify.d.authentication, expected);
  assert.equal(identifyMessage({ op: 0, d: { rpcVersion: 1 } }).d.authentication, undefined);
  assert.throws(() => identifyMessage({ op: 0, d: { authentication: { salt, challenge } } }, undefined));
  assert.deepEqual(requestMessage('SetCurrentProgramScene', { sceneName: 'BRB' }, '7'),
    { op: 6, d: { requestType: 'SetCurrentProgramScene', requestId: '7', requestData: { sceneName: 'BRB' } } });
});

// A fake OBS behind the WebSocket interface Node exposes: answers Hello, Identified,
// and every request with success, recording what was asked.
class FakeSocket {
  constructor() { this.listeners = {}; this.sent = []; FakeSocket.last = this;
    setTimeout(() => this.emit('message', { data: JSON.stringify({ op: 0, d: { rpcVersion: 1,
      authentication: { salt: 's', challenge: 'c' } } }) }), 0); }
  addEventListener(name, handler) { (this.listeners[name] ??= []).push(handler); }
  emit(name, event) { for (const handler of this.listeners[name] ?? []) handler(event); }
  send(text) {
    const message = JSON.parse(text); this.sent.push(message);
    if (message.op === 1) setTimeout(() => this.emit('message', { data: JSON.stringify({ op: 2, d: {} }) }), 0);
    if (message.op === 6) setTimeout(() => this.emit('message', { data: JSON.stringify({ op: 7,
      d: { requestId: message.d.requestId, requestStatus: { result: true } } }) }), 0);
  }
  close() {}
}

test('the kill switch phase moves OBS to the BRB scene and back, and the caption follows the file', async () => {
  const root = await mkdtemp(path.join(tmpdir(), 'revia-obs-'));
  try {
    const statePath = path.join(root, 'avatar_state.json');
    const captionPath = path.join(root, 'caption.txt');
    const client = new ObsClient({ liveScene: 'Live', brbScene: 'BRB', captionSource: 'Caption' },
      { password: 'pw', WebSocketImpl: FakeSocket, log: () => {} });
    await client.connect();
    const socket = FakeSocket.last;
    assert.equal(socket.sent[0].op, 1);
    assert.equal(socket.sent[0].d.authentication, authenticationResponse('pw', 's', 'c'));

    await writeFile(statePath, JSON.stringify({ phase: 'idle' }));
    await client.sync({ statePath, captionPath });
    assert.equal(socket.sent.filter(m => m.op === 6).length, 0, 'idle switched a scene');

    await writeFile(statePath, JSON.stringify({ phase: 'brb' }));
    await writeFile(captionPath, 'Cats, obviously. Filtered.\n');
    await client.sync({ statePath, captionPath });
    const requests = socket.sent.filter(m => m.op === 6).map(m => m.d);
    assert.deepEqual(requests.map(r => r.requestType), ['SetCurrentProgramScene', 'SetInputSettings']);
    assert.equal(requests[0].requestData.sceneName, 'BRB');
    assert.equal(requests[1].requestData.inputSettings.text, 'Cats, obviously. Filtered.');

    await client.sync({ statePath, captionPath });
    assert.equal(socket.sent.filter(m => m.op === 6).length, 2, 'an unchanged state was re-sent');

    await writeFile(statePath, JSON.stringify({ phase: 'speaking' }));
    await client.sync({ statePath, captionPath });
    const back = socket.sent.filter(m => m.op === 6).at(-1).d;
    assert.equal(back.requestData.sceneName, 'Live');
  } finally { await rm(root, { recursive: true, force: true }); }
});
