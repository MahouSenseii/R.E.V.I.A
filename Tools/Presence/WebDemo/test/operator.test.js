import test from 'node:test';
import assert from 'node:assert/strict';
import http from 'node:http';
import { once } from 'node:events';
import { mkdtemp, writeFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { loadSettings, readStatus, nativeEnvironment } from '../operator/config.js';

const localToken = 'local-test-only-'.repeat(4);
const hostToken = 'host-test-only-'.repeat(4);
async function settings(t, native = '', bridge = '') {
  const dir = await mkdtemp(join(tmpdir(), 'revia-operator-'));
  t.after(() => rm(dir, { recursive: true, force: true }));
  await writeFile(join(dir, '.env.revia'), `REVIA_WEB_ENABLED=1\nREVIA_WEB_PORT=17864\nREVIA_WEB_LOCAL_TOKEN=${localToken}\n${native}`);
  await writeFile(join(dir, '.env.bridge'), `NODE_ENV=production\nREVIA_WEB_HOST_ID=revia-public\nREVIA_WEB_HOST_TOKEN=${hostToken}\nREVIA_WEB_RELAY_URL=wss://relay.example/v1/host\n${bridge}`);
  return dir;
}
test('operator shares the native token with bridge and sends only native web settings to desktop', async t => {
  const config = await loadSettings(await settings(t), true);
  assert.equal(config.bridge.localToken, localToken);
  assert.equal(config.bridge.localUrl, 'http://127.0.0.1:17864');
  assert.equal(config.relayOrigin, 'https://relay.example');
  const env = nativeEnvironment(config, { PATH: 'keep', REVIA_WEB_HOST_TOKEN: hostToken, REVIA_WEB_INVITE_SHA256: 'secret-hash' });
  assert.equal(env.PATH, 'keep');
  assert.equal(env.REVIA_WEB_LOCAL_TOKEN, localToken);
  assert.equal(env.REVIA_WEB_ENABLED, '1');
  assert.equal(env.REVIA_WEB_HOST_TOKEN, undefined);
  assert.equal(env.REVIA_WEB_INVITE_SHA256, undefined);
});
test('operator fails closed on disabled native mode, placeholder secrets, unsafe URL and conflicting local settings', async t => {
  for (const [native, bridge] of [
    ['REVIA_WEB_ENABLED=0', ''], ['REVIA_WEB_PORT=0', ''], ['REVIA_WEB_LOCAL_TOKEN=REPLACE_ME', ''],
    ['', 'NODE_ENV=development'], ['', 'REVIA_WEB_RELAY_URL=ws://relay.example/v1/host'],
    ['', `REVIA_WEB_LOCAL_TOKEN=${hostToken}`], ['', 'REVIA_WEB_LOCAL_URL=http://127.0.0.1:9999'],
    ['', 'NODE_OPTIONS=--inspect'], ['', `REVIA_WEB_HOST_TOKEN=${localToken}`],
  ]) await assert.rejects(loadSettings(await settings(t, native, bridge), true));
});
test('native-only settings do not require a bridge file', async t => {
  const dir = await settings(t);
  await rm(join(dir, '.env.bridge'));
  assert.equal((await loadSettings(dir, false)).port, 17864);
});
test('Windows entry point validates files with spaces without exposing credentials', { skip: process.platform !== 'win32' }, async t => {
  const dir = await settings(t);
  const script = fileURLToPath(new URL('../WebDemo.ps1', import.meta.url));
  const child = spawn('powershell.exe', ['-NoProfile', '-File', script, 'check', '-ConfigDirectory', dir], { windowsHide: true });
  let output = '';
  child.stdout.on('data', bytes => output += bytes);
  child.stderr.on('data', bytes => output += bytes);
  const [code] = await once(child, 'exit');
  assert.equal(code, 0, output);
  assert.match(output, /no network connection made/);
  assert.ok(!output.includes(localToken) && !output.includes(hostToken));
});
test('status uses native authentication, refuses redirects and rejects malformed status', async t => {
  let path, authorization, origin;
  const server = http.createServer((req, res) => {
    path = req.url; authorization = req.headers.authorization; origin = req.headers.origin;
    if (req.url === '/redirect') return res.writeHead(302, { Location: '/web/v1/status' }).end();
    res.setHeader('Content-Type', 'application/json');
    res.end(JSON.stringify(req.url === '/bad' ? { state: 'made-up' } : { state: 'online' }));
  });
  server.listen(0, '127.0.0.1'); await once(server, 'listening');
  t.after(() => { server.closeAllConnections(); server.close(); });
  const base = `http://127.0.0.1:${server.address().port}`;
  assert.equal(await readStatus(base + '/web/v1/status', localToken), 'online');
  assert.equal(path, '/web/v1/status'); assert.equal(authorization, `Bearer ${localToken}`); assert.equal(origin, undefined);
  assert.equal(await readStatus(base + '/v1/status'), 'online');
  assert.equal(authorization, undefined);
  await assert.rejects(readStatus(base + '/redirect', localToken));
  await assert.rejects(readStatus(base + '/bad'));
});
