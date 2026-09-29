import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createBridge, validateConfig } from './bridge.mjs';

const fake = new URL('./fakeStdioServer.mjs', import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1');

test('the config is checked before anything is spawned', () => {
  assert.throws(() => validateConfig({ servers: { 'bad id!': { command: 'x' } } }), /not a server id/);
  assert.throws(() => validateConfig({ servers: { obs: {} } }), /needs a command/);
  assert.equal(validateConfig({}).port, 8760);
});

test('a stdio server is reached through one HTTP endpoint per id, with notifications acknowledged', async () => {
  const bridge = createBridge({ port: 0, token: 't', servers: { fake: { command: process.execPath, args: [fake] } } });
  const port = await bridge.listen(0);
  const base = `http://127.0.0.1:${port}`;
  const headers = { 'Content-Type': 'application/json', Authorization: 'Bearer t' };
  try {
    const health = await (await fetch(`${base}/health`, { headers })).json();
    assert.deepEqual(health.servers, ['fake']);
    const unauthorized = await fetch(`${base}/fake`, { method: 'POST', body: '{}' });
    assert.equal(unauthorized.status, 401);
    const missing = await fetch(`${base}/nope`, { method: 'POST', headers, body: JSON.stringify({ jsonrpc: '2.0', id: 1, method: 'tools/list' }) });
    assert.equal(missing.status, 404);

    const initialized = await fetch(`${base}/fake`, { method: 'POST', headers, body: JSON.stringify({ jsonrpc: '2.0', id: 1, method: 'initialize', params: {} }) });
    assert.equal(initialized.status, 200);
    assert.equal((await initialized.json()).result.serverInfo.name, 'fake-stdio');
    const notified = await fetch(`${base}/fake`, { method: 'POST', headers, body: JSON.stringify({ jsonrpc: '2.0', method: 'notifications/initialized' }) });
    assert.equal(notified.status, 202);
    const listed = await (await fetch(`${base}/fake`, { method: 'POST', headers, body: JSON.stringify({ jsonrpc: '2.0', id: 2, method: 'tools/list' }) })).json();
    assert.equal(listed.result.tools[0].name, 'echo');
    const called = await (await fetch(`${base}/fake`, { method: 'POST', headers, body: JSON.stringify({ jsonrpc: '2.0', id: 3, method: 'tools/call', params: { name: 'echo', arguments: { text: 'hi' } } }) })).json();
    assert.equal(called.result.content[0].text, '{"text":"hi"}');
  } finally {
    bridge.close();
  }
});
