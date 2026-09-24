import test from 'node:test';
import assert from 'node:assert/strict';
import http from 'node:http';
import { randomUUID } from 'node:crypto';
import { once } from 'node:events';
import { WebSocketServer } from 'ws';
import { createRelay } from '../relay/server.js';
import { createBridge, bridgeConfig } from '../bridge/client.js';

const hostToken = 'host-secret-'.repeat(4), localToken = 'local-secret-'.repeat(4), inviteCode = 'invite-secret-'.repeat(4);
const wait = ms => new Promise(r => setTimeout(r, ms));
async function until(fn) { for (let i = 0; i < 200; i++) { if (await fn()) return; await wait(10); } assert.fail('State did not arrive'); }
async function setup(t, behavior = {}) {
  const calls = [], turns = new Map(); let readiness = 'online';
  const native = http.createServer(async (req, res) => {
    assert.equal(req.headers.authorization, `Bearer ${localToken}`);
    let raw = ''; for await (const chunk of req) raw += chunk;
    const body = raw ? JSON.parse(raw) : undefined;
    calls.push({ path: req.url, body });
    res.setHeader('Content-Type', 'application/json');
    if (req.url === '/web/v1/status') return res.end(JSON.stringify({ state: readiness }));
    if (req.url === '/web/v1/turn') {
      assert.deepEqual(Object.keys(body).sort(), ['deadline', 'epoch', 'requestId', 'sessionId', 'text', 'version']);
      turns.set(body.requestId, res);
      if (behavior.hold) return;
      if (behavior.malformed) return res.end(JSON.stringify({ requestId: body.requestId, state: 'completed', text: 'unsafe', diagnostic: 'private path' }));
      if (behavior.huge) return res.end('x'.repeat(40000));
      return res.end(JSON.stringify({ requestId: body.requestId, state: 'completed', text: `Reply: ${body.text}` }));
    }
    if (req.url === '/web/v1/cancel') {
      const response = turns.get(body.requestId);
      if (behavior.slowCancel) await wait(50);
      response?.end(JSON.stringify({ requestId: body.requestId, state: 'cancelled' }));
      return res.end(JSON.stringify({ requestId: body.requestId, state: 'cancelled' }));
    }
    if (req.url === '/web/v1/end') return res.end(JSON.stringify({ state: 'ended' }));
    assert.fail('Bridge forwarded an unapproved native route');
  });
  native.listen(0, '127.0.0.1'); await once(native, 'listening');
  const relay = createRelay({ mode: 'development', bind: '127.0.0.1', origins: ['http://127.0.0.1:9000'], hostId: 'demo', hostToken, inviteCode, limits: { issuePerMinute: 20 } });
  await relay.listen(0);
  const base = `http://127.0.0.1:${relay.server.address().port}`;
  const bridge = createBridge({ mode: 'development', relayUrl: base.replace('http:', 'ws:') + '/v1/host', hostId: 'demo', hostToken, localUrl: `http://127.0.0.1:${native.address().port}`, localToken, heartbeatMs: 50, reconnectMinMs: 20, reconnectMaxMs: 50 });
  t.after(async () => { await bridge.stop(); await relay.close(); native.closeAllConnections(); await new Promise(r => native.close(r)); });
  bridge.start();
  async function api(path, token, body, method = body ? 'POST' : 'GET') {
    const r = await fetch(base + path, { method, headers: { ...(token ? { Authorization: `Bearer ${token}` } : {}), ...(body ? { 'Content-Type': 'application/json' } : {}) }, ...(body ? { body: JSON.stringify(body) } : {}) });
    return { status: r.status, body: r.status === 204 ? null : await r.json() };
  }
  await until(async () => (await api('/v1/status')).body.state === 'online');
  const session = async () => (await api('/v1/sessions', null, { inviteCode })).body;
  const submit = async (s, text = 'Hello') => (await api('/v1/messages', s.token, { text, idempotencyKey: randomUUID() })).body;
  return { relay, bridge, calls, api, session, submit, readiness: state => { readiness = state; } };
}

test('real relay and bridge preserve narrow serialized guest requests and literal final replies', async t => {
  const { api, session, submit, calls } = await setup(t);
  const a = await session(), b = await session();
  const one = await submit(a, 'guest A 😀'), two = await submit(b, '<script>guest B</script>');
  await until(async () => (await api('/v1/messages/' + two.requestId, b.token)).body.state === 'completed');
  assert.equal((await api('/v1/messages/' + one.requestId, a.token)).body.text, 'Reply: guest A 😀');
  assert.equal((await api('/v1/messages/' + two.requestId, b.token)).body.text, 'Reply: <script>guest B</script>');
  const requests = calls.filter(c => c.path === '/web/v1/turn');
  assert.equal(requests.length, 2); assert.notEqual(requests[0].body.sessionId, requests[1].body.sessionId);
  assert.equal(requests[0].body.epoch, requests[1].body.epoch);
});

test('bridge cancellation reaches native before acknowledging and session end deletes context', async t => {
  const { api, session, submit, calls } = await setup(t, { hold: true, slowCancel: true });
  const a = await session(), b = await session();
  const one = await submit(a); await until(() => calls.some(c => c.path === '/web/v1/turn'));
  await submit(b);
  await api('/v1/messages/' + one.requestId + '/cancel', a.token, undefined, 'POST');
  await until(() => calls.some(c => c.path === '/web/v1/cancel'));
  assert.equal(calls.filter(c => c.path === '/web/v1/turn').length, 1);
  await until(() => calls.filter(c => c.path === '/web/v1/turn').length === 2);
  await api('/v1/session', a.token, undefined, 'DELETE');
  await until(() => calls.some(c => c.path === '/web/v1/end' && c.body.sessionId === a.sessionId));
});

test('native malformed or excessive results are sanitized to failure', async t => {
  for (const behavior of [{ malformed: true }, { huge: true }]) {
    await t.test(JSON.stringify(behavior), async t => {
      const { api, session, submit } = await setup(t, behavior);
      const s = await session(), r = await submit(s);
      await until(async () => (await api('/v1/messages/' + r.requestId, s.token)).body.state === 'failed');
      assert.deepEqual((await api('/v1/messages/' + r.requestId, s.token)).body, { requestId: r.requestId, state: 'failed' });
    });
  }
});

test('owner pause cancels native work and exposes paused readiness without raw diagnostics', async t => {
  const { api, session, submit, calls, readiness } = await setup(t, { hold: true });
  const s = await session(); const r = await submit(s);
  await until(() => calls.some(c => c.path === '/web/v1/turn'));
  readiness('paused');
  await until(async () => (await api('/v1/status')).body.state === 'paused');
  await until(() => calls.some(c => c.path === '/web/v1/cancel'));
  assert.equal((await api('/v1/messages/' + r.requestId, s.token)).body.state, 'cancelled');
});

test('authoritative native offline cancels work and permanently stops the connector until explicitly restarted', async t => {
  const { api, session, submit, calls, readiness } = await setup(t, { hold: true });
  const s = await session(); await submit(s);
  await until(() => calls.some(c => c.path === '/web/v1/turn'));
  readiness('offline');
  await until(() => calls.some(c => c.path === '/web/v1/cancel'));
  await until(() => calls.some(c => c.path === '/web/v1/end'));
  await until(async () => (await api('/v1/status')).body.state === 'offline');
  const callCount = calls.length;
  await wait(200);
  assert.equal(calls.length, callCount, 'Stopped connector must cease readiness polling');
  readiness('online'); await wait(200);
  assert.equal((await api('/v1/status')).body.state, 'offline', 'Re-enabling native requires starting a fresh connector process');
});

test('owner disable during relay outage stops reconnect attempts before the relay returns', async t => {
  const { relay, readiness } = await setup(t);
  const port = relay.server.address().port;
  await relay.close(); readiness('offline'); await wait(150);
  const replacement = createRelay({ mode: 'development', bind: '127.0.0.1', origins: ['http://127.0.0.1:9000'], hostId: 'demo', hostToken, inviteCode });
  let upgrades = 0; replacement.server.on('upgrade', () => { upgrades++; });
  await replacement.listen(port); t.after(() => replacement.close());
  await wait(150);
  assert.equal(upgrades, 0, 'Disabled native must stop attempts before a new relay connection is made');
});

test('connector shutdown cancels active native work, ends guest contexts and leaves relay offline', async t => {
  const { api, session, submit, calls, bridge } = await setup(t, { hold: true });
  const s = await session(); await submit(s);
  await until(() => calls.some(c => c.path === '/web/v1/turn'));
  await bridge.stop();
  assert(calls.some(c => c.path === '/web/v1/cancel'));
  assert(calls.some(c => c.path === '/web/v1/end' && c.body.sessionId === s.sessionId));
  await until(async () => (await api('/v1/status')).body.state === 'offline');
});

test('relay restart reconnects with a fresh epoch and never replays the previous native turn', async t => {
  const { relay, api, session, submit, calls } = await setup(t, { hold: true });
  const s = await session(), one = await submit(s);
  await until(() => calls.some(c => c.path === '/web/v1/turn'));
  const port = relay.server.address().port;
  await relay.close();
  await until(() => calls.some(c => c.path === '/web/v1/end' && c.body.sessionId === s.sessionId));
  const replacement = createRelay({ mode: 'development', bind: '127.0.0.1', origins: ['http://127.0.0.1:9000'], hostId: 'demo', hostToken, inviteCode });
  await replacement.listen(port); t.after(() => replacement.close());
  await until(async () => (await api('/v1/status')).body.state === 'online');
  assert.equal((await api('/v1/messages/' + one.requestId, s.token)).status, 401);
  assert.equal(calls.filter(c => c.path === '/web/v1/turn').length, 1);
  const fresh = await session(); await submit(fresh);
  await until(() => calls.filter(c => c.path === '/web/v1/turn').length === 2);
  const turns = calls.filter(c => c.path === '/web/v1/turn');
  assert.notEqual(turns[0].body.epoch, turns[1].body.epoch);
});

test('production configuration forbids plaintext remote transport, unsafe local endpoints and weak secrets', () => {
  const base = { mode: 'production', relayUrl: 'wss://demo.example/v1/host', hostId: 'demo', hostToken, localToken };
  assert.equal(bridgeConfig(base).localUrl, 'http://127.0.0.1:17864');
  for (const change of [{ relayUrl: 'ws://demo.example/v1/host' }, { localUrl: 'http://example.com' }, { localUrl: 'http://127.0.0.1:17864/private' }, { relayUrl: 'wss://demo.example/v1/host?token=secret' }, { localToken: 'short' }, { hostToken: 'short' }]) assert.throws(() => bridgeConfig({ ...base, ...change }));
  assert.throws(() => bridgeConfig({ ...base, mode: 'development', relayUrl: 'ws://example.com/v1/host' }));
});

test('relay clock skew is accepted only within five seconds and native execution is capped locally', async t => {
  for (const [offset, accepted] of [[122000, true], [126000, false]]) {
    await t.test(`${offset}ms future deadline`, async t => {
      let payload, receivedAt, closed = false;
      const native = http.createServer(async (req, res) => {
        let raw = ''; for await (const chunk of req) raw += chunk;
        res.setHeader('Content-Type', 'application/json');
        if (req.url === '/web/v1/status') return res.end(JSON.stringify({ state: 'online' }));
        if (req.url === '/web/v1/end') return res.end(JSON.stringify({ state: 'ended' }));
        assert.equal(req.url, '/web/v1/turn'); payload = JSON.parse(raw); receivedAt = Date.now();
        res.end(JSON.stringify({ requestId: payload.requestId, state: 'completed', text: 'Complete' }));
      });
      native.listen(0, '127.0.0.1'); await once(native, 'listening');
      const relay = new WebSocketServer({ port: 0, host: '127.0.0.1' }); await once(relay, 'listening');
      relay.on('connection', ws => {
        const epoch = randomUUID(); let sent = false;
        ws.on('close', () => { closed = true; });
        ws.on('message', data => {
          if (JSON.parse(data).type !== 'ready' || sent) return;
          sent = true;
          ws.send(JSON.stringify({ version: 1, epoch, type: 'turn', sessionId: randomUUID(), requestId: randomUUID(), text: 'Clock test', deadline: Date.now() + offset }));
        });
        ws.send(JSON.stringify({ version: 1, epoch, type: 'welcome' }));
      });
      const bridge = createBridge({ mode: 'development', relayUrl: `ws://127.0.0.1:${relay.address().port}/v1/host`, localUrl: `http://127.0.0.1:${native.address().port}`, hostId: 'demo', hostToken, localToken });
      t.after(async () => { await bridge.stop(); for (const ws of relay.clients) ws.terminate(); await new Promise(resolve => relay.close(resolve)); native.closeAllConnections(); await new Promise(resolve => native.close(resolve)); });
      bridge.start(); await until(() => payload || closed);
      if (accepted) {
        assert(payload, 'A deadline inside the skew tolerance must reach native');
        assert(payload.deadline <= receivedAt + 120000, 'Native deadline must not exceed a local 120-second execution budget');
        assert(payload.deadline > receivedAt + 117000, 'A tolerated positive skew must preserve the usable budget');
      } else {
        assert.equal(payload, undefined, 'An excessive future deadline must never reach native');
        assert.equal(closed, true);
      }
    });
  }
});
