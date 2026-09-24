import http from 'node:http';
import { randomBytes, randomUUID } from 'node:crypto';
import { WebSocketServer, WebSocket } from 'ws';
import { relayConfig, hash, secretMatches } from './config.js';
import { sourceAddress } from './proxy.js';
import { MAX_BODY_BYTES, MAX_FRAME_BYTES, frame, id, sessionBody, messageBody, TERMINAL } from '../protocol/index.js';

export function createRelay(input) {
  const config = input?.origins instanceof Set ? input : relayConfig(input);
  const L = config.limits;
  const sessions = new Map(); const tokens = new Map(); const rates = new Map();
  const rateSalt = randomBytes(32).toString('hex');
  let host = null, active = null, queue = [], retainedBytes = 0, closing = false;
  let day = Math.floor(Date.now() / 86400000), workload = 0;
  function refreshDay() { const d = Math.floor(Date.now() / 86400000); if (d !== day) { day = d; workload = 0; } }
  function rate(key, cap) {
    const now = Date.now(); let r = rates.get(key);
    if (!r || r.until <= now) {
      if (!r && rates.size >= L.maxRateKeys) return false;
      r = { count: 0, until: now + 60000 }; rates.set(key, r);
    }
    return ++r.count <= cap;
  }
  function send(value) {
    if (!host || host.ws.readyState !== WebSocket.OPEN) return false;
    if (host.ws.bufferedAmount > MAX_FRAME_BYTES * 4) { host.ws.terminate(); return false; }
    host.ws.send(JSON.stringify({ version: 1, epoch: host.epoch, ...value })); return true;
  }
  function state() {
    if (!host) return 'offline';
    if (host.state !== 'online') return host.state;
    return active || queue.length ? 'busy' : 'online';
  }
  function publicRequest(r) { return { requestId: r.requestId, state: r.state, ...(r.state === 'completed' ? { text: r.result } : {}) }; }
  function finish(r, resultState, result) {
    if (TERMINAL.includes(r.state)) return;
    retainedBytes -= Buffer.byteLength(r.text); r.text = '';
    r.state = resultState;
    if (resultState === 'completed') { r.result = result; retainedBytes += Buffer.byteLength(result); }
    const s = sessions.get(r.sessionId); if (s?.pending === r) s.pending = null;
  }
  function cancel(r, reason) {
    if (TERMINAL.includes(r.state)) return;
    finish(r, reason);
    queue = queue.filter(item => item !== r);
    if (active === r) {
      r.cancelAt = Date.now();
      send({ type: 'cancel', sessionId: r.sessionId, requestId: r.requestId });
    }
  }
  function end(s) {
    if (s.pending) cancel(s.pending, 'cancelled');
    send({ type: 'end', sessionId: s.sessionId });
    for (const r of s.requests.values()) retainedBytes -= Buffer.byteLength(r.text) + Buffer.byteLength(r.result ?? '');
    sessions.delete(s.sessionId); tokens.delete(s.tokenHash);
  }
  function loseHost(ws) {
    if (host?.ws !== ws) return;
    host = null; active = null; queue = []; sessions.clear(); tokens.clear(); retainedBytes = 0;
  }
  function dispatch() {
    if (!host || host.state !== 'online' || active || closing) return;
    while (queue.length) {
      const r = queue.shift();
      if (r.deadline <= Date.now()) { finish(r, 'expired'); continue; }
      if (!sessions.has(r.sessionId) || TERMINAL.includes(r.state)) continue;
      active = r; r.state = 'running';
      send({ type: 'turn', sessionId: r.sessionId, requestId: r.requestId, text: r.text, deadline: r.deadline });
      break;
    }
  }
  function sweep() {
    const now = Date.now(); refreshDay();
    for (const [key, r] of rates) if (r.until <= now) rates.delete(key);
    for (const s of sessions.values()) if (s.expiresAt <= now || s.lastActivity + L.idleMs <= now) end(s);
    for (const r of queue) if (r.deadline <= now) cancel(r, 'expired');
    if (active && !TERMINAL.includes(active.state) && active.deadline <= now) cancel(active, 'expired');
    if (host && (now - host.lastReady > L.heartbeatMs || (active?.cancelAt && now - active.cancelAt > L.cancelAckMs))) { const ws = host.ws; loseHost(ws); ws.terminate(); }
    dispatch();
  }
  const timer = setInterval(sweep, L.sweepMs); timer.unref();
  function reply(res, code, value) { if (res.destroyed) return; res.writeHead(code, { 'Content-Type': 'application/json; charset=utf-8' }); res.end(value === null ? '' : JSON.stringify(value)); }
  const fail = (res, status, error) => reply(res, status, { error });
  async function body(req) {
    if (!/^application\/json(?:\s*;\s*charset=utf-8)?$/i.test(req.headers['content-type'] ?? '') || req.headers['content-encoding']) throw { code: 400 };
    if (Number(req.headers['content-length']) > MAX_BODY_BYTES) { req.resume(); throw { code: 413 }; }
    let size = 0; const chunks = [];
    for await (const chunk of req) {
      size += chunk.length;
      if (size > MAX_BODY_BYTES) { req.resume(); throw { code: 413 }; }
      chunks.push(chunk);
    }
    try { return JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(Buffer.concat(chunks))); } catch { throw { code: 400 }; }
  }
  const server = http.createServer({ maxHeaderSize: 8192, requestTimeout: 10000, headersTimeout: 10000, keepAliveTimeout: 5000 }, async (req, res) => {
    res.setHeader('Cache-Control', 'no-store'); res.setHeader('Vary', 'Origin'); res.setHeader('X-Content-Type-Options', 'nosniff');
    const origin = req.headers.origin;
    if (origin && !config.origins.has(origin)) return fail(res, 403, 'forbidden');
    if (origin) res.setHeader('Access-Control-Allow-Origin', origin);
    if (req.method === 'OPTIONS') {
      res.setHeader('Access-Control-Allow-Methods', 'GET, POST, DELETE');
      res.setHeader('Access-Control-Allow-Headers', 'Authorization, Content-Type');
      return reply(res, 204, null);
    }
    const path = req.url;
    const source = hash(rateSalt + sourceAddress(req, config.proxyTrust));
    if (path === '/healthz' && req.method === 'GET') return reply(res, 200, { ok: true });
    if (!rate('all:' + source, L.readsPerMinute)) return fail(res, 429, 'rate_limited');
    sweep();
    if (path === '/v1/status' && req.method === 'GET') {
      if (!rate('status:' + source, L.statusPerMinute)) return fail(res, 429, 'rate_limited');
      return reply(res, 200, { state: state() });
    }
    try {
      if (path === '/v1/sessions' && req.method === 'POST') {
        if (!rate('issue:' + source, L.issuePerMinute)) return fail(res, 429, 'rate_limited');
        const b = await body(req);
        if (!sessionBody(b)) return fail(res, 400, 'invalid_request');
        if (!secretMatches(b.inviteCode, config.inviteHash)) return fail(res, 403, 'admission_denied');
        if (!host || !['online', 'busy'].includes(state())) return fail(res, 503, 'unavailable');
        if (sessions.size >= L.maxSessions) {
          const closed = [...sessions.values()].find(s => s.closed);
          if (closed) end(closed);
        }
        if (sessions.size >= L.maxSessions || workload >= L.dailyCap || retainedBytes >= L.maxRetainedBytes) return fail(res, 429, 'capacity');
        const sessionId = randomUUID(); const token = randomBytes(32).toString('base64url'); const tokenHash = hash(token); const now = Date.now();
        const s = { sessionId, tokenHash, expiresAt: now + L.totalMs, lastActivity: now, requests: new Map(), pending: null };
        sessions.set(sessionId, s); tokens.set(tokenHash, s);
        return reply(res, 201, { sessionId, token, expiresAt: s.expiresAt });
      }
      const auth = req.headers.authorization ?? '';
      const token = /^Bearer ([A-Za-z0-9_-]{43})$/.exec(auth)?.[1];
      const s = token && tokens.get(hash(token));
      if (!s) return fail(res, 401, 'unauthorized');
      if (path === '/v1/session' && req.method === 'DELETE') { end(s); return reply(res, 204, null); }
      if (path === '/v1/messages' && req.method === 'POST') {
        const b = await body(req);
        sweep();
        if (tokens.get(s.tokenHash) !== s || s.closed) return fail(res, 401, 'unauthorized');
        if (!messageBody(b)) return fail(res, 400, 'invalid_request');
        const old = s.requests.get(b.idempotencyKey);
        if (old) return old.contentHash === hash(b.text) ? reply(res, 202, publicRequest(old)) : fail(res, 409, 'idempotency_conflict');
        if (!host || !['online', 'busy'].includes(host.state)) return fail(res, 503, 'unavailable');
        if (s.pending || (active?.sessionId === s.sessionId)) return fail(res, 409, 'pending_request');
        if (queue.length >= L.maxWaiting && (active || host.state !== 'online')) return fail(res, 429, 'capacity');
        if (s.requests.size >= L.maxRequests || workload >= L.dailyCap || retainedBytes + Buffer.byteLength(b.text) + 16384 > L.maxRetainedBytes) return fail(res, 429, 'capacity');
        if (!rate('submit:' + s.sessionId, L.submissionsPerMinute) || !rate('source:' + source, L.sourceSubmissionsPerMinute)) return fail(res, 429, 'rate_limited');
        const r = { sessionId: s.sessionId, requestId: randomUUID(), state: 'queued', text: b.text, contentHash: hash(b.text), deadline: Date.now() + L.requestMs };
        s.requests.set(b.idempotencyKey, r); s.pending = r; s.lastActivity = Date.now();
        workload++; retainedBytes += Buffer.byteLength(b.text); queue.push(r); dispatch();
        return reply(res, 202, publicRequest(r));
      }
      const match = /^\/v1\/messages\/([^/]+)(\/cancel)?$/.exec(path);
      if (match && id(match[1])) {
        const r = [...s.requests.values()].find(value => value.requestId === match[1]);
        if (!r) return fail(res, 404, 'not_found');
        if (match[2] && req.method === 'POST') { cancel(r, 'cancelled'); dispatch(); return reply(res, 200, publicRequest(r)); }
        if (!match[2] && req.method === 'GET') return reply(res, 200, publicRequest(r));
      }
      return fail(res, 404, 'not_found');
    } catch (error) { return fail(res, error.code === 413 ? 413 : 400, error.code === 413 ? 'body_too_large' : 'invalid_request'); }
  });
  server.maxConnections = 128;
  const wss = new WebSocketServer({ noServer: true, maxPayload: MAX_FRAME_BYTES, perMessageDeflate: false });
  server.on('upgrade', (req, socket, head) => {
    const source = hash(rateSalt + sourceAddress(req, config.proxyTrust));
    const token = /^Bearer (.+)$/.exec(req.headers.authorization ?? '')?.[1];
    if (closing || req.url !== '/v1/host' || req.headers.origin || req.headers['x-revia-host'] !== config.hostId || !rate('upgrade:' + source, 10) || !secretMatches(token, config.hostTokenHash) || host) {
      socket.end('HTTP/1.1 403 Forbidden\r\nConnection: close\r\n\r\n'); return;
    }
    wss.handleUpgrade(req, socket, head, ws => {
      host = { ws, epoch: randomUUID(), state: 'starting', lastReady: Date.now() };
      let frameWindow = Date.now(), frameCount = 0;
      send({ type: 'welcome' });
      ws.on('error', () => {});
      ws.on('close', () => loseHost(ws));
      ws.on('message', (data, binary) => {
        if (host?.ws !== ws) return;
        if (Date.now() - frameWindow > 60000) { frameWindow = Date.now(); frameCount = 0; }
        let f;
        try { if (binary || ++frameCount > 300) throw Error(); f = JSON.parse(data.toString()); if (!frame(f, 'host') || f.epoch !== host.epoch) throw Error(); }
        catch { loseHost(ws); ws.close(1008, 'Invalid protocol'); return; }
        if (f.type === 'ready') {
          host.lastReady = Date.now(); host.state = f.state;
          if (['paused', 'offline', 'unavailable', 'starting'].includes(f.state)) {
            for (const s of sessions.values()) s.closed = true;
            for (const r of queue) cancel(r, 'failed');
            if (active) cancel(active, 'cancelled');
          }
          dispatch(); return;
        }
        const r = active;
        if (!r || f.sessionId !== r.sessionId || f.requestId !== r.requestId) return;
        if (f.type === 'cancelled' && r.cancelAt) { active = null; dispatch(); return; }
        if (f.type === 'result' && !r.cancelAt) {
          if (r.deadline <= Date.now()) finish(r, 'expired');
          else if (f.state === 'completed' && retainedBytes + Buffer.byteLength(f.text) > L.maxRetainedBytes) finish(r, 'failed');
          else finish(r, f.state, f.text);
          active = null; dispatch();
        }
      });
    });
  });
  return {
    server,
    listen: port => new Promise((resolve, reject) => { server.once('error', reject); server.listen(port, config.bind, resolve); }),
    close: async () => {
      if (closing) return; closing = true; clearInterval(timer);
      for (const s of [...sessions.values()]) end(s);
      for (const ws of wss.clients) ws.terminate();
      await new Promise(resolve => wss.close(resolve));
      server.closeAllConnections(); await new Promise(resolve => server.close(resolve));
    }
  };
}
