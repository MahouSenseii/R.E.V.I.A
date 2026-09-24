import test from 'node:test';
import assert from 'node:assert/strict';
import { once } from 'node:events';
import { randomUUID } from 'node:crypto';
import http from 'node:http';
import WebSocket from 'ws';
import { createRelay } from '../relay/server.js';

const hostToken = 'host-test-secret-'.repeat(3);
const inviteCode = 'invite-test-secret-'.repeat(3);
const origin = 'http://127.0.0.1:9000';
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
async function setup(t, limits = {}) {
  const relay = createRelay({ mode: 'development', bind: '127.0.0.1', hostToken, hostId: 'demo', inviteCode, origins: [origin], limits });
  await relay.listen(0);
  const url = `http://127.0.0.1:${relay.server.address().port}`;
  t.after(() => relay.close());
  async function api(path, { token, body, method = 'GET', headers = {} } = {}) {
    const response = await fetch(url + path, { method, headers: { Origin: origin, ...(token ? { Authorization: `Bearer ${token}` } : {}), ...(body !== undefined ? { 'Content-Type': 'application/json' } : {}), ...headers }, ...(body !== undefined ? { body: JSON.stringify(body) } : {}) });
    return { status: response.status, headers: response.headers, body: response.status === 204 ? null : await response.json() };
  }
  async function host() {
    const ws = new WebSocket(url.replace('http:', 'ws:') + '/v1/host', { headers: { Authorization: `Bearer ${hostToken}`, 'X-Revia-Host': 'demo' } });
    const frames = [];
    ws.on('message', data => frames.push(JSON.parse(data)));
    await once(ws, 'open');
    await until(() => frames.length);
    const epoch = frames[0].epoch;
    const send = body => ws.send(JSON.stringify({ version: 1, epoch, ...body }));
    send({ type: 'ready', state: 'online' });
    await until(async () => (await api('/v1/status')).body.state === 'online');
    return { ws, frames, epoch, send };
  }
  async function session() {
    const r = await api('/v1/sessions', { method: 'POST', body: { inviteCode } });
    assert.equal(r.status, 201);
    return r.body;
  }
  const submit = (s, text = 'Hello', idempotencyKey = randomUUID()) => api('/v1/messages', { method: 'POST', token: s.token, body: { text, idempotencyKey } });
  return { relay, api, host, session, submit, url };
}
async function until(fn) {
  for (let n = 0; n < 100; n++) { if (await fn()) return; await delay(10); }
  assert.fail('Timed out waiting for observable state');
}

test('availability requires authenticated application readiness; CORS is exact and private responses are not cached', async t => {
  const { api, host } = await setup(t);
  assert.deepEqual((await api('/v1/status')).body, { state: 'offline' });
  assert.equal((await api('/v1/sessions', { method: 'POST', body: { inviteCode } })).status, 503);
  assert.equal((await api('/v1/status', { headers: { Origin: origin + '.evil' } })).status, 403);
  const h = await host();
  const r = await api('/v1/status');
  assert.equal(r.headers.get('cache-control'), 'no-store');
  assert.equal(r.headers.get('access-control-allow-origin'), origin);
  h.send({ type: 'ready', state: 'paused' });
  await until(async () => (await api('/v1/status')).body.state === 'paused');
});

test('invite admission and strict unknown fields reject forged roles and oversize Unicode before dispatch', async t => {
  const { api, host, session } = await setup(t);
  const h = await host();
  assert.equal((await api('/v1/sessions', { method: 'POST', body: { inviteCode: 'wrong' } })).status, 403);
  assert.equal((await api('/v1/sessions', { method: 'POST', body: { inviteCode, role: 'owner' } })).status, 400);
  const s = await session();
  for (const body of [{ text: 'x', idempotencyKey: randomUUID(), role: 'owner' }, { text: '😀'.repeat(2001), idempotencyKey: randomUUID() }, { text: '', idempotencyKey: randomUUID() }, { text: 'x', idempotencyKey: '../escape' }]) {
    assert.equal((await api('/v1/messages', { method: 'POST', token: s.token, body })).status, 400);
  }
  assert.equal(h.frames.filter(f => f.type === 'turn').length, 0);
});

test('session-scoped idempotency never executes twice, even after completion; cross-guest IDs authorize uniformly', async t => {
  const { api, host, session, submit } = await setup(t);
  const h = await host(); const a = await session(); const b = await session();
  const key = randomUUID();
  const first = await submit(a, '<script>alert(1)</script>', key);
  assert.equal(first.status, 202);
  assert.equal((await submit(a, '<script>alert(1)</script>', key)).body.requestId, first.body.requestId);
  assert.equal((await submit(a, 'different', key)).status, 409);
  const path = '/v1/messages/' + first.body.requestId;
  assert.deepEqual((await api(path, { token: b.token })).body, { error: 'not_found' });
  assert.equal((await api(path + '/cancel', { method: 'POST', token: b.token })).status, 404);
  await until(() => h.frames.some(f => f.type === 'turn'));
  const turn = h.frames.find(f => f.type === 'turn');
  assert.equal(turn.text, '<script>alert(1)</script>');
  h.send({ type: 'result', sessionId: a.sessionId, requestId: turn.requestId, state: 'completed', text: '<b>literal</b>' });
  await until(async () => (await api(path, { token: a.token })).body.state === 'completed');
  assert.deepEqual((await api(path, { token: a.token })).body, { requestId: turn.requestId, state: 'completed', text: '<b>literal</b>' });
  assert.equal((await submit(a, '<script>alert(1)</script>', key)).body.requestId, turn.requestId);
  assert.equal(h.frames.filter(f => f.type === 'turn').length, 1);
});

test('cancellation holds the execution slot until native acknowledgement and suppresses late completion', async t => {
  const { api, host, session, submit } = await setup(t);
  const h = await host(); const a = await session(); const b = await session();
  const one = (await submit(a)).body; const two = (await submit(b)).body;
  assert.equal(two.state, 'queued');
  assert.equal((await submit(a, 'overlap')).status, 409);
  const path = '/v1/messages/' + one.requestId;
  await api(path + '/cancel', { method: 'POST', token: a.token });
  await until(() => h.frames.some(f => f.type === 'cancel'));
  assert.equal(h.frames.filter(f => f.type === 'turn').length, 1);
  h.send({ type: 'result', sessionId: a.sessionId, requestId: one.requestId, state: 'completed', text: 'must not leak' });
  await delay(20);
  assert.equal((await api(path, { token: a.token })).body.state, 'cancelled');
  assert.equal(h.frames.filter(f => f.type === 'turn').length, 1);
  h.send({ type: 'cancelled', sessionId: a.sessionId, requestId: one.requestId });
  await until(() => h.frames.filter(f => f.type === 'turn').length === 2);
});

test('disconnect revokes tokens, cancels outstanding work, and stale epochs cannot publish after reconnect', async t => {
  const { api, host, session, submit } = await setup(t);
  const h = await host(); const a = await session(); const one = (await submit(a)).body;
  h.ws.terminate();
  await until(async () => (await api('/v1/status')).body.state === 'offline');
  assert.equal((await api('/v1/messages/' + one.requestId, { token: a.token })).status, 401);
  const next = await host(); assert.notEqual(h.epoch, next.epoch);
  next.ws.send(JSON.stringify({ version: 1, type: 'result', epoch: h.epoch, sessionId: a.sessionId, requestId: one.requestId, state: 'completed', text: 'old' }));
  await until(async () => (await api('/v1/status')).body.state === 'offline');
  assert.equal(next.frames.filter(f => f.type === 'turn').length, 0);
});

test('FIFO admission bounds waiting turns, live sessions and retained requests', async t => {
  const { api, host, session, submit } = await setup(t, { maxSessions: 3, maxWaiting: 1, issuePerMinute: 20 });
  const h = await host(); const a = await session(); const b = await session(); const c = await session();
  assert.equal((await api('/v1/sessions', { method: 'POST', body: { inviteCode } })).status, 429);
  assert.equal((await submit(a)).status, 202);
  assert.equal((await submit(b)).status, 202);
  assert.equal((await submit(c)).status, 429);
  assert.equal(h.frames.filter(f => f.type === 'turn').length, 1);
});

test('deadlines, idle expiry and session end release memory and reject old tokens', async t => {
  const { api, host, session, submit } = await setup(t, { requestMs: 70, sweepMs: 10, idleMs: 180, totalMs: 300 });
  const h = await host(); const a = await session(); const one = (await submit(a)).body;
  await until(() => h.frames.some(f => f.type === 'cancel'));
  assert.equal((await api('/v1/messages/' + one.requestId, { token: a.token })).body.state, 'expired');
  h.send({ type: 'cancelled', sessionId: a.sessionId, requestId: one.requestId });
  await api('/v1/session', { method: 'DELETE', token: a.token });
  assert.equal((await submit(a)).status, 401);
  await until(() => h.frames.some(f => f.type === 'end'));
  const b = await session(); await delay(220);
  assert.equal((await submit(b)).status, 401);
});

test('rate and global workload admission caps apply to issuance and submissions without trusting forwarded headers', async t => {
  const { api, host, session, submit } = await setup(t, { submissionsPerMinute: 1, dailyCap: 1, issuePerMinute: 2 });
  await host(); const a = await session();
  assert.equal((await submit(a)).status, 202);
  assert.equal((await api('/v1/sessions', { method: 'POST', body: { inviteCode } })).status, 429);
  assert.equal((await api('/v1/sessions', { method: 'POST', body: { inviteCode }, headers: { 'X-Forwarded-For': '1.2.3.4' } })).status, 429);
});

test('malformed JSON, whole-body limits and binary/unknown-version host frames fail closed', async t => {
  const { api, host, url } = await setup(t);
  let response = await fetch(url + '/v1/sessions', { method: 'POST', headers: { Origin: origin, 'Content-Type': 'application/json' }, body: '{' });
  assert.equal(response.status, 400);
  response = await fetch(url + '/v1/sessions', { method: 'POST', headers: { Origin: origin, 'Content-Type': 'application/json' }, body: 'x'.repeat(17000) });
  assert.equal(response.status, 413);
  const h = await host(); h.ws.send(JSON.stringify({ version: 999, type: 'ready', epoch: h.epoch, state: 'online' }));
  await until(async () => (await api('/v1/status')).body.state === 'offline');
});

test('heartbeat loss closes host and exposes offline', async t => {
  const { api, host } = await setup(t, { heartbeatMs: 60, sweepMs: 10 });
  await host();
  await until(async () => (await api('/v1/status')).body.state === 'offline');
});

test('owner pause closes old sessions for further work while preserving cancellation polling', async t => {
  const { api, host, session, submit } = await setup(t);
  const h = await host(), s = await session(); const first = (await submit(s)).body;
  h.send({ type: 'ready', state: 'paused' });
  await until(async () => (await api('/v1/status')).body.state === 'paused');
  assert.equal((await api('/v1/messages/' + first.requestId, { token: s.token })).body.state, 'cancelled');
  h.send({ type: 'cancelled', sessionId: s.sessionId, requestId: first.requestId });
  h.send({ type: 'ready', state: 'online' });
  await until(async () => (await api('/v1/status')).body.state === 'online');
  assert.equal((await submit(s, 'cannot resume ended native context')).status, 401);
  assert.equal((await submit(await session())).status, 202);
});

test('native busy state accepts only the bounded waiting queue', async t => {
  const { api, host, session, submit } = await setup(t, { maxWaiting: 1 });
  const h = await host(), a = await session(), b = await session();
  h.send({ type: 'ready', state: 'busy' });
  await until(async () => (await api('/v1/status')).body.state === 'busy');
  assert.equal((await submit(a)).body.state, 'queued');
  assert.equal((await submit(b)).status, 429);
  assert.equal(h.frames.filter(f => f.type === 'turn').length, 0);
  h.send({ type: 'ready', state: 'online' });
  await until(() => h.frames.filter(f => f.type === 'turn').length === 1);
});

test('session deletion while a slow submission body arrives prevents admission and dispatch', async t => {
  const { api, host, session, url } = await setup(t);
  const h = await host(), s = await session();
  const serialized = JSON.stringify({ text: 'too late', idempotencyKey: randomUUID() });
  let request;
  const result = new Promise((resolve, reject) => {
    request = http.request(url + '/v1/messages', { method: 'POST', headers: { Authorization: `Bearer ${s.token}`, 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(serialized) } }, response => { response.resume(); response.on('end', () => resolve(response.statusCode)); });
    request.on('error', reject); request.write(serialized.slice(0, 5));
  });
  await delay(20);
  await api('/v1/session', { method: 'DELETE', token: s.token });
  request.end(serialized.slice(5));
  assert.equal(await result, 401);
  assert.equal(h.frames.filter(f => f.type === 'turn').length, 0);
});

test('closed sessions do not prevent fresh admission after owner resumes at full capacity', async t => {
  const { api, host, session, submit } = await setup(t, { maxSessions: 1 });
  const h = await host(), s = await session();
  h.send({ type: 'ready', state: 'paused' });
  await until(async () => (await api('/v1/status')).body.state === 'paused');
  h.send({ type: 'ready', state: 'online' });
  await until(async () => (await api('/v1/status')).body.state === 'online');
  const next = await session();
  assert.notEqual(next.sessionId, s.sessionId);
  assert.equal((await submit(s)).status, 401);
});

test('submission frequency and retained-request caps cannot be reset with a new idempotency key', async t => {
  for (const limits of [{ submissionsPerMinute: 1 }, { maxRequests: 1 }]) {
    await t.test(JSON.stringify(limits), async t => {
      const { api, host, session, submit } = await setup(t, limits);
      const h = await host(), s = await session(), first = (await submit(s)).body;
      h.send({ type: 'result', sessionId: s.sessionId, requestId: first.requestId, state: 'completed', text: 'done' });
      await until(async () => (await api('/v1/messages/' + first.requestId, { token: s.token })).body.state === 'completed');
      assert.equal((await submit(s, 'again')).status, 429);
      assert.equal(h.frames.filter(f => f.type === 'turn').length, 1);
    });
  }
});

test('missing, wrong and browser-origin host credentials cannot open the outbound host channel', async t => {
  const { url, api } = await setup(t);
  for (const headers of [{}, { Authorization: 'Bearer incorrect', 'X-Revia-Host': 'demo' }, { Authorization: `Bearer ${hostToken}`, 'X-Revia-Host': 'demo', Origin: origin }]) {
    const status = await new Promise((resolve, reject) => {
      const ws = new WebSocket(url.replace('http:', 'ws:') + '/v1/host', { headers });
      ws.on('error', () => {});
      ws.on('open', () => { ws.terminate(); reject(Error('Unauthorized WebSocket accepted')); });
      ws.on('unexpected-response', (request, response) => { resolve(response.statusCode); response.destroy(); request.destroy(); });
    });
    assert.equal(status, 403);
  }
  assert.equal((await api('/v1/status')).body.state, 'offline');
});

export { setup, until };
