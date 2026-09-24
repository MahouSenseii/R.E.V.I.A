import test from 'node:test';
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { relayConfig, hash } from '../relay/config.js';

const hostToken = 'host-secret-'.repeat(4), localToken = 'local-secret-'.repeat(4);
test('relay public startup fails closed without admission, exact origins or safe limits', () => {
  const base = { hostToken, hostId: 'demo', inviteCode: 'invite-secret-'.repeat(4), origins: ['https://portfolio.example'] };
  assert.equal(relayConfig(base).mode, 'production');
  for (const change of [{ inviteCode: undefined }, { origins: ['*'] }, { origins: ['http://example.com'] }, { origins: ['https://example.com/'] }, { mode: 'development', bind: '0.0.0.0' }, { limits: { maxSessions: 11 } }, { limits: { imaginary: 1 } }]) assert.throws(() => relayConfig({ ...base, ...change }));
});

test('configuration checks validate only locally and never print credentials', async () => {
  for (const [file, env] of [
    ['relay/main.js', { REVIA_WEB_HOST_TOKEN_SHA256: hash(hostToken), REVIA_WEB_INVITE_SHA256: hash('invite-secret-'.repeat(4)), REVIA_WEB_ORIGINS: 'https://portfolio.example', REVIA_WEB_HOST_ID: 'demo' }],
    ['bridge/main.js', { REVIA_WEB_RELAY_URL: 'wss://demo.invalid/v1/host', REVIA_WEB_HOST_TOKEN: hostToken, REVIA_WEB_LOCAL_TOKEN: localToken, REVIA_WEB_HOST_ID: 'demo' }]
  ]) {
    const child = spawn(process.execPath, [file, '--check'], { cwd: fileURLToPath(new URL('..', import.meta.url)), env: { ...process.env, ...env }, windowsHide: true });
    let output = ''; child.stdout.on('data', d => { output += d; }); child.stderr.on('data', d => { output += d; });
    const code = await new Promise(resolve => child.once('exit', resolve));
    assert.equal(code, 0); assert.match(output, /no network connection made/); assert(!output.includes(hostToken)); assert(!output.includes(localToken));
  }
});

test('connector remains running across failed connection attempts until explicitly stopped', async t => {
  const child = spawn(process.execPath, ['bridge/main.js'], {
    cwd: fileURLToPath(new URL('..', import.meta.url)), windowsHide: true,
    env: { ...process.env, NODE_ENV: 'development', REVIA_WEB_RELAY_URL: 'ws://127.0.0.1:1/v1/host', REVIA_WEB_HOST_TOKEN: hostToken, REVIA_WEB_LOCAL_TOKEN: localToken, REVIA_WEB_HOST_ID: 'demo' }
  });
  t.after(() => child.kill());
  await new Promise(resolve => setTimeout(resolve, 700));
  assert.equal(child.exitCode, null, 'The connector exited instead of backing off and reconnecting');
});
