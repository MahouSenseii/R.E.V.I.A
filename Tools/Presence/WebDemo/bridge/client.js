import WebSocket from 'ws';
import { frame, statusBody, nativeResult, MAX_FRAME_BYTES, object } from '../protocol/index.js';
import { loopback } from '../relay/config.js';

export function bridgeConfig(input = {}) {
  const mode = input.mode ?? 'production';
  if (!['production', 'development'].includes(mode)) throw Error('Invalid mode');
  const relay = new URL(input.relayUrl ?? 'invalid');
  if (relay.pathname !== '/v1/host' || relay.search || relay.hash || relay.username || relay.password || !(relay.protocol === 'wss:' || (mode === 'development' && relay.protocol === 'ws:' && loopback(relay.hostname)))) throw Error('Relay must be a secure /v1/host WebSocket URL');
  const localUrl = input.localUrl ?? 'http://127.0.0.1:17864'; const local = new URL(localUrl);
  if (local.protocol !== 'http:' || !loopback(local.hostname) || local.pathname !== '/' || local.search || local.hash || local.username || local.password) throw Error('Native endpoint must be a literal loopback HTTP origin');
  if (![input.hostToken, input.localToken].every(v => typeof v === 'string' && Buffer.byteLength(v) >= 32 && v.length <= 256 && /^[\x21-\x7e]+$/.test(v))) throw Error('Separate credentials of at least 32 bytes are required');
  if (input.hostToken === input.localToken) throw Error('Host and native credentials must differ');
  if (!/^[a-zA-Z0-9_-]{1,64}$/.test(input.hostId ?? '')) throw Error('Invalid host ID');
  const heartbeatMs = input.heartbeatMs ?? 3000, reconnectMinMs = input.reconnectMinMs ?? 500, reconnectMaxMs = input.reconnectMaxMs ?? 30000;
  if (!Number.isSafeInteger(heartbeatMs) || heartbeatMs < 20 || heartbeatMs > 5000 || !Number.isSafeInteger(reconnectMinMs) || reconnectMinMs < 10 || !Number.isSafeInteger(reconnectMaxMs) || reconnectMaxMs < reconnectMinMs || reconnectMaxMs > 60000) throw Error('Invalid connector timing');
  return { mode, relayUrl: relay.href, localUrl: local.origin, hostId: input.hostId, hostToken: input.hostToken, localToken: input.localToken, heartbeatMs, reconnectMinMs, reconnectMaxMs };
}
export function bridgeConfigFromEnv(env = process.env) {
  return bridgeConfig({ mode: env.NODE_ENV === 'development' ? 'development' : 'production', relayUrl: env.REVIA_WEB_RELAY_URL, localUrl: env.REVIA_WEB_LOCAL_URL, hostId: env.REVIA_WEB_HOST_ID, hostToken: env.REVIA_WEB_HOST_TOKEN, localToken: env.REVIA_WEB_LOCAL_TOKEN });
}

export function createBridge(input) {
  const config = bridgeConfig(input);
  let stopped = true, ws = null, epoch = null, active = null, heartbeat = null, retryTimer = null, attempts = 0, checking = false;
  let cleanupPromise = Promise.resolve(), stopPromise = null;
  const sessions = new Map(); const endings = new Map();
  async function native(path, body, timeout = 2000) {
    const response = await fetch(config.localUrl + path, {
      method: body ? 'POST' : 'GET', redirect: 'error', signal: AbortSignal.timeout(Math.max(1, timeout)),
      headers: { Authorization: `Bearer ${config.localToken}`, ...(body ? { 'Content-Type': 'application/json' } : {}) },
      ...(body ? { body: JSON.stringify(body) } : {})
    });
    if (!response.ok) { await response.body?.cancel(); throw Error('Native refused'); }
    let bytes = 0; const chunks = [];
    for await (const chunk of response.body) {
      bytes += chunk.length;
      if (bytes > MAX_FRAME_BYTES) throw Error('Native response too large');
      chunks.push(chunk);
    }
    return JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(Buffer.concat(chunks)));
  }
  function send(connection, value) {
    if (connection !== ws || connection.readyState !== WebSocket.OPEN) return;
    if (connection.bufferedAmount > MAX_FRAME_BYTES * 4) { connection.terminate(); return; }
    connection.send(JSON.stringify({ version: 1, epoch, ...value }));
  }
  async function ready(connection) {
    if (checking || stopped || connection !== ws || !epoch) return;
    checking = true;
    try {
      const result = await native('/web/v1/status');
      const state = statusBody(result) ? result.state : 'unavailable';
      send(connection, { type: 'ready', state });
      // Native reserves offline for authoritative owner disable. Model failure
      // remains unavailable, and paused mode can resume on the same connection.
      if (state === 'offline') await stop();
    }
    catch { send(connection, { type: 'ready', state: 'unavailable' }); }
    finally { checking = false; }
  }
  async function cancel(task) {
    if (task.cancelPromise) return task.cancelPromise;
    task.cancelled = true;
    task.cancelPromise = (async () => {
      const result = await native('/web/v1/cancel', { version: 1, epoch: task.epoch, sessionId: task.sessionId, requestId: task.requestId });
      if (!nativeResult(result, task.requestId) || result.state !== 'cancelled') throw Error('Invalid cancellation acknowledgement');
      // Native acknowledges the stop request first. The original turn must unwind
      // before releasing the relay's single global execution slot.
      await Promise.race([task.promise, new Promise((_, reject) => {
        const timer = setTimeout(() => reject(Error('Cancellation did not settle')), 2500); timer.unref();
        task.promise.finally(() => clearTimeout(timer));
      })]);
    })();
    return task.cancelPromise;
  }
  async function end(sessionId, sessionEpoch) {
    if (endings.has(sessionId)) return endings.get(sessionId);
    const ending = (async () => {
      if (active?.sessionId === sessionId) await cancel(active);
      const result = await native('/web/v1/end', { version: 1, epoch: sessionEpoch, sessionId });
      if (!object(result, ['state']) || result.state !== 'ended') throw Error('Invalid session acknowledgement');
      sessions.delete(sessionId);
    })();
    endings.set(sessionId, ending);
    try { await ending; } finally { endings.delete(sessionId); }
  }
  async function cleanup() {
    if (active) { try { await cancel(active); } catch { /* Native's absolute deadline still applies. */ } }
    const results = await Promise.allSettled([...sessions].map(([sessionId, s]) => end(sessionId, s.epoch)));
    return results.every(result => result.status === 'fulfilled');
  }
  function scheduleReconnect() {
    if (stopped) return;
    const cap = Math.min(config.reconnectMaxMs, config.reconnectMinMs * 2 ** Math.min(attempts++, 10));
    retryTimer = setTimeout(async () => {
      const clean = await cleanup();
      if (stopped) return;
      try {
        const status = await native('/web/v1/status');
        if (statusBody(status) && status.state === 'offline') { await stop(); return; }
      } catch { /* Native may recover; only an authoritative offline stops us. */ }
      if (stopped) return;
      if (!clean || active) { scheduleReconnect(); return; }
      connect();
    }, Math.floor(cap / 2 + Math.random() * cap / 2));
    // Keep the connector process alive while the hosted relay is unavailable.
  }
  function connect() {
    if (stopped) return;
    const connection = new WebSocket(config.relayUrl, { headers: { Authorization: `Bearer ${config.hostToken}`, 'X-Revia-Host': config.hostId }, maxPayload: MAX_FRAME_BYTES, perMessageDeflate: false, handshakeTimeout: 5000, followRedirects: false });
    ws = connection; epoch = null;
    connection.on('error', () => {});
    const helloTimeout = setTimeout(() => { if (!epoch) connection.terminate(); }, 5000); helloTimeout.unref();
    connection.on('close', () => {
      clearTimeout(helloTimeout); clearInterval(heartbeat);
      if (ws !== connection) return;
      ws = null; epoch = null;
      cleanupPromise = cleanup();
      cleanupPromise.finally(scheduleReconnect);
    });
    let frameWindow = Date.now(), frameCount = 0;
    connection.on('message', (data, binary) => {
      let f;
      try {
        if (Date.now() - frameWindow > 60000) { frameWindow = Date.now(); frameCount = 0; }
        if (binary || ++frameCount > 300) throw Error();
        f = JSON.parse(data.toString());
        if (!frame(f, 'relay') || (epoch && f.epoch !== epoch) || (!epoch && f.type !== 'welcome')) throw Error();
      } catch { connection.close(1008, 'Invalid protocol'); return; }
      if (f.type === 'welcome') {
        if (epoch) { connection.close(1008, 'Duplicate welcome'); return; }
        epoch = f.epoch; attempts = 0; clearTimeout(helloTimeout);
        ready(connection); heartbeat = setInterval(() => ready(connection), config.heartbeatMs); heartbeat.unref(); return;
      }
      if (f.type === 'cancel') {
        const task = active;
        if (!task || task.sessionId !== f.sessionId || task.requestId !== f.requestId) {
          // A result and cancellation can cross in flight after native unwinds.
          if (sessions.get(f.sessionId)?.seen.has(f.requestId)) send(connection, { type: 'cancelled', sessionId: f.sessionId, requestId: f.requestId });
          return;
        }
        cancel(task).then(() => send(connection, { type: 'cancelled', sessionId: f.sessionId, requestId: f.requestId })).catch(() => connection.terminate()); return;
      }
      if (f.type === 'end') {
        if (sessions.has(f.sessionId)) end(f.sessionId, f.epoch).catch(() => connection.terminate());
        return;
      }
      if (f.type === 'turn') {
        const previous = sessions.get(f.sessionId);
        if (active || endings.has(f.sessionId) || previous?.seen.has(f.requestId) || (previous && previous.seen.size >= 32) || (!previous && sessions.size >= 10) || f.deadline > Date.now() + 125000) { connection.close(1008, 'Invalid dispatch'); return; }
        // Allow bounded relay/PC clock skew without extending local execution.
        const localDeadline = Math.min(f.deadline, Date.now() + 120000);
        const session = previous ?? { epoch: f.epoch, seen: new Set() }; session.seen.add(f.requestId); sessions.set(f.sessionId, session);
        const task = { ...f, cancelled: false, promise: null, cancelPromise: null }; active = task;
        task.promise = (async () => {
          let result = { requestId: f.requestId, state: 'expired' };
          if (localDeadline > Date.now()) {
            try {
              result = await native('/web/v1/turn', { version: 1, epoch: f.epoch, sessionId: f.sessionId, requestId: f.requestId, text: f.text, deadline: localDeadline }, localDeadline - Date.now() + 2000);
              if (!nativeResult(result, f.requestId)) throw Error('Invalid native result');
            } catch { result = { requestId: f.requestId, state: 'failed' }; }
          }
          if (active === task) active = null;
          if (!task.cancelled) send(connection, { type: 'result', sessionId: f.sessionId, ...result });
        })();
      }
    });
  }
  function stop() {
    if (stopPromise) return stopPromise;
    stopped = true; clearTimeout(retryTimer); clearInterval(heartbeat);
    stopPromise = (async () => {
      await cleanupPromise; await cleanup();
      if (ws) { const connection = ws; ws = null; connection.terminate(); }
    })();
    return stopPromise;
  }
  return { start() { if (!stopped || stopPromise) return; stopped = false; connect(); }, stop };
}
