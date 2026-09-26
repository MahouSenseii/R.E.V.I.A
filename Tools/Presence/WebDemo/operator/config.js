import { readFile } from 'node:fs/promises';
import { parseEnv } from 'node:util';
import { join } from 'node:path';
import { bridgeConfigFromEnv } from '../bridge/client.js';
import { statusBody, MAX_FRAME_BYTES } from '../protocol/index.js';

async function environmentFile(directory, name, allowed) {
  const values = parseEnv(await readFile(join(directory, name), 'utf8'));
  if (Object.keys(values).some(key => !allowed.includes(key))) throw Error('Unexpected environment setting');
  return values;
}

// Files are parsed as data, never executed or printed. Explicit files are the
// authority, so a stale inherited development setting cannot change transport.
export async function loadSettings(directory, withBridge = false) {
  const native = await environmentFile(directory, '.env.revia', ['REVIA_WEB_ENABLED', 'REVIA_WEB_PORT', 'REVIA_WEB_LOCAL_TOKEN']);
  const port = Number(native.REVIA_WEB_PORT);
  const token = native.REVIA_WEB_LOCAL_TOKEN;
  if (native.REVIA_WEB_ENABLED !== '1' || !/^\d+$/.test(native.REVIA_WEB_PORT ?? '') || !Number.isInteger(port) || port < 1 || port > 65535 ||
      !/^[\x21-\x7e]{32,256}$/.test(token ?? '') || /replace[_-]?me|replace-with/i.test(token)) throw Error('Invalid native settings');
  const config = { port, token, localUrl: `http://127.0.0.1:${port}` };
  if (withBridge) {
    const values = await environmentFile(directory, '.env.bridge', ['NODE_ENV', 'REVIA_WEB_RELAY_URL', 'REVIA_WEB_HOST_ID', 'REVIA_WEB_HOST_TOKEN', 'REVIA_WEB_LOCAL_TOKEN', 'REVIA_WEB_LOCAL_URL']);
    if (values.NODE_ENV !== 'production' || /replace[_-]?me|replace-with/i.test(values.REVIA_WEB_HOST_TOKEN ?? '') ||
        (values.REVIA_WEB_LOCAL_TOKEN !== undefined && values.REVIA_WEB_LOCAL_TOKEN !== token) ||
        (values.REVIA_WEB_LOCAL_URL !== undefined && values.REVIA_WEB_LOCAL_URL !== config.localUrl)) throw Error('Conflicting or invalid bridge settings');
    config.bridge = bridgeConfigFromEnv({ ...values, REVIA_WEB_LOCAL_TOKEN: token, REVIA_WEB_LOCAL_URL: config.localUrl });
    const relay = new URL(config.bridge.relayUrl);
    if (relay.hostname.endsWith('.invalid')) throw Error('Replace the relay placeholder');
    config.relayOrigin = `https://${relay.host}`;
  }
  return config;
}

export function nativeEnvironment(config, inherited = process.env) {
  const env = Object.fromEntries(Object.entries(inherited).filter(([key]) => !key.toUpperCase().startsWith('REVIA_WEB_')));
  return { ...env, REVIA_WEB_ENABLED: '1', REVIA_WEB_PORT: String(config.port), REVIA_WEB_LOCAL_TOKEN: config.token };
}

export async function readStatus(url, token) {
  const response = await fetch(url, { redirect: 'error', signal: AbortSignal.timeout(5000), headers: token ? { Authorization: `Bearer ${token}` } : {} });
  if (!response.ok) { await response.body?.cancel(); throw Error('Status refused'); }
  let size = 0; const chunks = [];
  for await (const chunk of response.body) {
    size += chunk.length;
    if (size > MAX_FRAME_BYTES) throw Error('Status too large');
    chunks.push(chunk);
  }
  const body = JSON.parse(Buffer.concat(chunks).toString('utf8'));
  if (!statusBody(body)) throw Error('Invalid status');
  return body.state;
}
