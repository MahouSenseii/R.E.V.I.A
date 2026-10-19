import test from 'node:test';
import assert from 'node:assert/strict';
import { appendFile, chmod, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { validateOperation, allowedOrigin, publicAddress, InteractiveBrowser } from './interactiveHost.mjs';

const grant = { enabled: true, navigate: true, interact: true,
  approvedOrigins: ['https://example.com', 'http://127.0.0.1:8080'], allowLoopback: false,
  timeoutMs: 5000, maxTextBytes: 4096, maxElements: 40, maxValueBytes: 4096 };

test('interactive permissions do not inherit research or private network access', () => {
  assert.equal(allowedOrigin('https://example.com/page', grant), true);
  for (const url of ['http://example.com', 'https://example.com.evil.test', 'file:///x', 'javascript:alert(1)',
    'https://owner:secret@example.com', 'http://127.0.0.1:8080']) assert.equal(allowedOrigin(url, grant), false);
  assert.equal(allowedOrigin('http://127.0.0.1:8080/x', { ...grant, allowLoopback: true }), true);
  for (const ip of ['127.0.0.1', '10.0.0.1', '192.168.1.1', '169.254.1.1', '::1', 'fd00::1', '::ffff:127.0.0.1'])
    assert.equal(publicAddress(ip), false);
  assert.equal(publicAddress('8.8.8.8'), true);
});
test('typed effects require a current owned observation and bounded text', () => {
  const base = { operation: 'fill', url: 'https://example.com', session: 'owned', generation: 3, element: '3-1', value: 'text', grant };
  const current = { session: 'owned', generation: 3 };
  assert.equal(validateOperation(base, current), '');
  for (const patch of [{ session: 'other' }, { generation: 2 }, { element: '' }, { script: 'alert(1)' },
    { value: 'x'.repeat(4097) }, { grant: { ...grant, interact: false } }, { grant: { ...grant, enabled: false } }])
    assert.notEqual(validateOperation({ ...base, ...patch }, current), '');
});

// A scripted CDP peer on fds 3/4 standing in for Edge; every method it receives is logged with its pid and request id.
// 'stall' never answers Page.enable, as a renderer that crashed or hung while the owned browser started would not.
// 'paused-stall' pauses an admitted request on Fetch.enable and never answers it; 'paused-denied' answers Fetch.enable and
// then pauses a request to an origin the grant does not admit under the same interception id, as every new browser restarts them.
const fakeBrowser = `#!/usr/bin/env node
import { appendFileSync } from 'node:fs';
import net from 'node:net';
const mode = process.env.REVIA_FAKE_BROWSER, log = process.env.REVIA_FAKE_BROWSER_LOG;
const output = new net.Socket({ fd: 4, readable: false }); const write = message => output.write(JSON.stringify(message) + '\\0');
const paused = url => write({ method: 'Fetch.requestPaused', sessionId: 'owned', params: { requestId: 'interception-job-1', request: { url } } });
let buffer = '', url = 'about:blank';
new net.Socket({ fd: 3, writable: false }).setEncoding('utf8').on('data', chunk => {
  buffer += chunk;
  for (let end; (end = buffer.indexOf('\\0')) >= 0; buffer = buffer.slice(end + 1)) {
    const { id, method, params } = JSON.parse(buffer.slice(0, end));
    appendFileSync(log, process.pid + ' ' + method + ' ' + (params?.requestId ?? '') + '\\n');
    if (mode === 'stall' && method === 'Page.enable') continue;
    if (mode === 'paused-stall' && method === 'Fetch.enable') { paused('http://127.0.0.1:8080/admitted'); continue; }
    if (method === 'Page.navigate') url = params.url;
    const value = method !== 'Runtime.evaluate' ? undefined : params.expression === 'document.readyState' ? 'complete'
      : { url, title: 'Fake', text: 'Fake page', elements: [], signature: url };
    write({ id, result: { 'Target.getTargets': { targetInfos: [{ targetId: 'page', type: 'page', url: 'about:blank' }] },
      'Target.attachToTarget': { sessionId: 'owned' }, 'Page.getFrameTree': { frameTree: { frame: { id: 'main' } } },
      'Page.createIsolatedWorld': { executionContextId: 1 }, 'Runtime.evaluate': { result: { value } } }[method] ?? {} });
    if (mode === 'paused-denied' && method === 'Fetch.enable') paused('http://127.0.0.1:9999/denied');
    if (method === 'Browser.close') setTimeout(() => process.exit(0), 10);
  }
});
`;
async function withFakeBrowser(run) {
  const root = await mkdtemp(path.join(os.tmpdir(), 'revia-fake-browser-')); const executable = path.join(root, 'fake-browser.mjs');
  const log = path.join(root, 'cdp.log');
  await writeFile(executable, fakeBrowser); await chmod(executable, 0o755); await appendFile(log, '');
  const browser = new InteractiveBrowser(path.join(root, 'profile'), executable);
  const launches = async () => {
    const byPid = new Map();
    for (const line of (await readFile(log, 'utf8')).split('\n').filter(Boolean)) {
      const [pid, ...rest] = line.split(' '); byPid.set(pid, [...(byPid.get(pid) ?? []), rest.join(' ').trim()]);
    }
    return [...byPid];
  };
  process.env.REVIA_FAKE_BROWSER_LOG = log;
  try { await run(browser, launches, mode => { process.env.REVIA_FAKE_BROWSER = mode; }); }
  finally {
    delete process.env.REVIA_FAKE_BROWSER; delete process.env.REVIA_FAKE_BROWSER_LOG;
    await browser.stop();
    await rm(root, { recursive: true, force: true, maxRetries: 5, retryDelay: 100 });
  }
}
const fakeRequest = { operation: 'navigate', url: 'http://127.0.0.1:8080/',
  grant: { ...grant, approvedOrigins: ['http://127.0.0.1:8080'], allowLoopback: true, timeoutMs: 300 } };
const posixOnly = { skip: process.platform === 'win32' && 'the scripted browser needs a POSIX shebang', timeout: 20000 };

test('a browser that fails to start names the stalled command and is never reused without network admission', posixOnly,
  () => withFakeBrowser(async (browser, launches, mode) => {
  mode('stall');
  await assert.rejects(browser.operate(fakeRequest), /^Error: Browser operation timed out \(Page\.enable\)\.$/);
  assert.equal(browser.child, null, 'A browser that failed to start stayed in use.');
  const [[stalled]] = await launches();
  for (let waited = 0; waited < 2000; waited += 20) {
    try { process.kill(Number(stalled), 0); } catch { break; }
    await new Promise(resolve => setTimeout(resolve, 20));
  }
  assert.throws(() => process.kill(Number(stalled), 0), 'The browser that failed to start was left running.');
  mode('ok');
  const result = await browser.operate(fakeRequest);
  assert.equal(result.succeeded, true, result.message);
  const started = await launches();
  assert.equal(started.length, 2, 'The next operation did not start a fresh browser.');
  const methods = started[1][1];
  assert.ok(methods.includes('Fetch.enable') && methods.indexOf('Fetch.enable') < methods.indexOf('Page.navigate'),
    'The page was navigated before network admission was enabled.');
}));

test('an admission decision still pending for a browser that failed to start never reaches its replacement', posixOnly,
  () => withFakeBrowser(async (browser, launches, mode) => {
  const admit = browser.allowedNetwork.bind(browser); let release;
  browser.allowedNetwork = raw => raw.endsWith('/admitted') ? new Promise(resolve => { release = () => resolve(true); }) : admit(raw);
  mode('paused-stall');
  await assert.rejects(browser.operate(fakeRequest), /\(Fetch\.enable\)/);
  mode('paused-denied');
  const result = await browser.operate(fakeRequest);
  assert.equal(result.succeeded, true, result.message);
  assert.ok(release, 'The first browser never paused its admitted request.');
  release(); await new Promise(resolve => setTimeout(resolve, 200));
  const [, [, replacement]] = await launches();
  assert.ok(replacement.includes('Fetch.failRequest interception-job-1'), 'The replacement did not refuse its own request.');
  assert.ok(!replacement.includes('Fetch.continueRequest interception-job-1'), 'The failed browser continued the replacement\'s request.');
}));
