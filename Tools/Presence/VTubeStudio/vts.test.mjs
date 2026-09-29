import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, readFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import { request, mouthValue, VTubeStudioClient } from './vts.mjs';

// A fake VTube Studio behind the WebSocket interface: grants a token, authenticates
// it, reports two hotkeys, and records every request.
class FakeSocket {
  constructor() { this.listeners = {}; this.sent = []; FakeSocket.last = this; setTimeout(() => this.emit('open', {}), 0); }
  addEventListener(name, handler) { (this.listeners[name] ??= []).push(handler); }
  emit(name, event) { for (const handler of this.listeners[name] ?? []) handler(event); }
  send(text) {
    const message = JSON.parse(text); this.sent.push(message);
    const reply = data => setTimeout(() => this.emit('message', { data: JSON.stringify({ requestID: message.requestID,
      messageType: message.messageType.replace(/Request$/, 'Response'), data }) }), 0);
    if (message.messageType === 'AuthenticationTokenRequest') reply({ authenticationToken: 'tok-1' });
    else if (message.messageType === 'AuthenticationRequest') reply({ authenticated: message.data.authenticationToken === 'tok-1' });
    else if (message.messageType === 'HotkeysInCurrentModelRequest') reply({ modelName: 'Revia', availableHotkeys: [
      { name: 'Smile', hotkeyID: 'h-smile' }, { name: 'Angry', hotkeyID: 'h-angry' }] });
    else reply({});
  }
  close() {}
}

test('a request carries the API header and a correlation id', () => {
  assert.deepEqual(request('AuthenticationRequest', { a: 1 }, 'r1'),
    { apiName: 'VTubeStudioPublicAPI', apiVersion: '1.0', requestID: 'r1', messageType: 'AuthenticationRequest', data: { a: 1 } });
});

test('the mouth is shut when silent and moves at syllable rate when speaking', () => {
  assert.equal(mouthValue(false, 500), 0);
  const values = [];
  for (let ms = 0; ms < 1000; ms += 33) values.push(mouthValue(true, ms, () => 0.5));
  assert.ok(Math.max(...values) > 0.7 && Math.min(...values) < 0.2, 'the envelope did not open and close');
  assert.ok(values.every(value => value >= 0 && value <= 1));
});

test('the token is requested once, kept, and reused; expressions map to hotkeys; parameters are re-sent each tick', async () => {
  const root = await mkdtemp(path.join(tmpdir(), 'revia-vts-'));
  try {
    let clock = 10000;
    const config = { tokenPath: path.join(root, 'token'), expressionHotkeys: { happy: 'Smile', angry: 'Angry', sad: 'Missing' } };
    const client = new VTubeStudioClient(config, { WebSocketImpl: FakeSocket, log: () => {}, random: () => 0.5, now: () => clock });
    await client.connect();
    await client.authenticate();
    assert.equal((await readFile(config.tokenPath, 'utf8')), 'tok-1');
    const types = () => FakeSocket.last.sent.map(m => m.messageType);
    assert.deepEqual(types(), ['AuthenticationTokenRequest', 'AuthenticationRequest', 'HotkeysInCurrentModelRequest']);

    await client.applyState({ sequence: 1, phase: 'idle', expression: 'happy', attention: 'local user' });
    assert.equal(FakeSocket.last.sent.at(-1).messageType, 'HotkeyTriggerRequest');
    assert.equal(FakeSocket.last.sent.at(-1).data.hotkeyID, 'h-smile');
    await client.applyState({ sequence: 2, phase: 'idle', expression: 'sad' });
    assert.equal(types().filter(t => t === 'HotkeyTriggerRequest').length, 1, 'an unmapped expression triggered a hotkey');
    await client.applyState({ sequence: 3, phase: 'idle', expression: 'sad' });
    assert.equal(types().filter(t => t === 'HotkeyTriggerRequest').length, 1, 'an unchanged expression was re-triggered');

    const silent = await client.tick();
    assert.equal(silent.find(p => p.id === 'MouthOpen').value, 0);
    assert.equal(silent.find(p => p.id === 'EyeOpenLeft').value, 0, 'the first tick did not blink');
    clock += 200;
    const open = await client.tick();
    assert.equal(open.find(p => p.id === 'EyeOpenLeft').value, 1);

    await client.applyState({ sequence: 4, phase: 'speaking', expression: 'sad', attention: 'stream:live' });
    clock += 120;
    const talking = await client.tick();
    assert.ok(talking.find(p => p.id === 'MouthOpen').value > 0.3, 'the mouth stayed shut while speaking');
    assert.ok(talking.find(p => p.id === 'FaceAngleX').value > 5, 'she did not look toward the stream');
    const injected = FakeSocket.last.sent.filter(m => m.messageType === 'InjectParameterDataRequest');
    assert.equal(injected.length, 3);
    assert.equal(injected.at(-1).data.mode, 'set');

    // A second client with the saved token never asks for permission again.
    const again = new VTubeStudioClient(config, { WebSocketImpl: FakeSocket, log: () => {}, random: () => 0.5, now: () => clock });
    await again.connect();
    await again.authenticate();
    assert.equal(FakeSocket.last.sent[0].messageType, 'AuthenticationRequest');
  } finally { await rm(root, { recursive: true, force: true }); }
});
