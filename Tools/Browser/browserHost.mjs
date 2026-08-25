import { spawn } from 'node:child_process';
import { timingSafeEqual } from 'node:crypto';
import { lookup } from 'node:dns/promises';
import { access, readFile, rm } from 'node:fs/promises';
import http from 'node:http';
import net from 'node:net';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const SEARCH_HOME = 'https://duckduckgo.com/';
const MAX_REQUEST_BODY = 16 * 1024;
const MAX_URL_LENGTH = 4096;
const DNS_CACHE_MS = 60_000;
const dnsCache = new Map();

function sleep(milliseconds) {
  return new Promise((resolve) => setTimeout(resolve, Math.max(0, milliseconds)));
}

function parseArguments(argv) {
  const values = new Map();
  for (let index = 0; index < argv.length; index += 1) {
    const key = argv[index];
    if (!key.startsWith('--') || index + 1 >= argv.length) {
      throw new Error(`Invalid worker argument: ${key}`);
    }
    values.set(key.slice(2), argv[++index]);
  }
  const settings = {
    host: values.get('host') ?? '127.0.0.1',
    port: Number(values.get('port')),
    token: values.get('token') ?? '',
    profile: path.resolve(values.get('profile') ?? 'RuntimeData/Browser/Profile'),
    maxPages: Number(values.get('max-pages') ?? 3),
    stepDelayMs: Number(values.get('step-delay-ms') ?? 250),
  };
  if (settings.host !== '127.0.0.1') throw new Error('The browser worker may bind only to 127.0.0.1.');
  if (!Number.isInteger(settings.port) || settings.port < 1024 || settings.port > 65535) {
    throw new Error('The browser worker port is invalid.');
  }
  if (settings.token.length < 32 || settings.token.length > 256) {
    throw new Error('The browser worker token is invalid.');
  }
  settings.maxPages = Math.max(1, Math.min(5, Math.trunc(settings.maxPages) || 3));
  settings.stepDelayMs = Math.max(0, Math.min(3000, Math.trunc(settings.stepDelayMs) || 0));
  return settings;
}

function ipv4Parts(address) {
  if (net.isIP(address) !== 4) return null;
  return address.split('.').map((part) => Number(part));
}

export function isPublicIpAddress(input) {
  const address = input.toLowerCase().replace(/^\[|\]$/g, '');
  const ipv4 = ipv4Parts(address);
  if (ipv4) {
    const [a, b, c] = ipv4;
    if (a === 0 || a === 10 || a === 127 || a >= 224) return false;
    if (a === 100 && b >= 64 && b <= 127) return false;
    if (a === 169 && b === 254) return false;
    if (a === 172 && b >= 16 && b <= 31) return false;
    if (a === 192 && b === 168) return false;
    if (a === 192 && b === 0 && (c === 0 || c === 2)) return false;
    if (a === 198 && (b === 18 || b === 19 || b === 51)) return false;
    if (a === 203 && b === 0 && c === 113) return false;
    return true;
  }
  if (net.isIP(address) !== 6) return false;
  if (address === '::' || address === '::1') return false;
  if (address.startsWith('::ffff:')) {
    const mapped = address.slice(7);
    if (net.isIP(mapped) === 4) return isPublicIpAddress(mapped);
    const groups = mapped.split(':');
    if (groups.length === 2 && groups.every((group) => /^[0-9a-f]{1,4}$/.test(group))) {
      const high = Number.parseInt(groups[0], 16);
      const low = Number.parseInt(groups[1], 16);
      return isPublicIpAddress(`${high >> 8}.${high & 255}.${low >> 8}.${low & 255}`);
    }
    return false;
  }
  if (address.startsWith('fc') || address.startsWith('fd')) return false;
  if (/^fe[89ab]/.test(address) || address.startsWith('ff')) return false;
  if (address.startsWith('2001:db8')) return false;
  if (address.startsWith('64:ff9b::')) return false;
  return true;
}

export function basicPublicUrlPolicy(raw, { httpsOnly = false } = {}) {
  if (typeof raw !== 'string' || raw.length === 0 || raw.length > MAX_URL_LENGTH) return null;
  let parsed;
  try {
    parsed = new URL(raw);
  } catch {
    return null;
  }
  if (httpsOnly ? parsed.protocol !== 'https:' : !['http:', 'https:'].includes(parsed.protocol)) {
    return null;
  }
  if (parsed.username || parsed.password) return null;
  if (parsed.port && !((parsed.protocol === 'https:' && parsed.port === '443') ||
      (parsed.protocol === 'http:' && parsed.port === '80'))) return null;
  const host = parsed.hostname.toLowerCase().replace(/^\[|\]$/g, '').replace(/\.$/, '');
  if (!host || host === 'localhost' || host.endsWith('.localhost') || host.endsWith('.local') ||
      host.endsWith('.internal') || host.endsWith('.lan')) return null;
  if (net.isIP(host) && !isPublicIpAddress(host)) return null;
  return parsed;
}

async function addressesFor(host) {
  const now = Date.now();
  const cached = dnsCache.get(host);
  if (cached && cached.expiresAt > now) return cached.addresses;
  const addresses = (await lookup(host, { all: true, verbatim: true })).map((entry) => entry.address);
  dnsCache.set(host, { addresses, expiresAt: now + DNS_CACHE_MS });
  return addresses;
}

async function allowedPublicUrl(raw, options = {}) {
  const parsed = basicPublicUrlPolicy(raw, options);
  if (!parsed) return null;
  const host = parsed.hostname.replace(/^\[|\]$/g, '');
  if (net.isIP(host)) return parsed;
  try {
    const addresses = await addressesFor(host);
    if (addresses.length === 0 || addresses.some((address) => !isPublicIpAddress(address))) return null;
    return parsed;
  } catch {
    return null;
  }
}

export function unwrapDuckDuckGoUrl(raw) {
  try {
    const parsed = new URL(raw, SEARCH_HOME);
    if (parsed.hostname.endsWith('duckduckgo.com') && parsed.pathname.startsWith('/l/')) {
      const target = parsed.searchParams.get('uddg');
      return target || raw;
    }
    return parsed.href;
  } catch {
    return raw;
  }
}

async function firstExisting(paths) {
  for (const candidate of paths) {
    try {
      await access(candidate);
      return candidate;
    } catch {
      // Try the next standard installation path.
    }
  }
  return null;
}

class CdpClient {
  constructor(url) {
    this.url = url;
    this.socket = null;
    this.nextId = 1;
    this.pending = new Map();
    this.listeners = new Map();
  }

  async connect(timeoutMs = 10_000) {
    this.socket = new WebSocket(this.url);
    await new Promise((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error('CDP WebSocket connection timed out.')), timeoutMs);
      this.socket.addEventListener('open', () => { clearTimeout(timer); resolve(); }, { once: true });
      this.socket.addEventListener('error', () => { clearTimeout(timer); reject(new Error('CDP WebSocket connection failed.')); }, { once: true });
    });
    this.socket.addEventListener('message', (event) => this.#receive(String(event.data)));
    this.socket.addEventListener('close', () => {
      for (const pending of this.pending.values()) pending.reject(new Error('CDP WebSocket closed.'));
      this.pending.clear();
    });
  }

  #receive(raw) {
    let message;
    try { message = JSON.parse(raw); } catch { return; }
    if (message.id) {
      const pending = this.pending.get(message.id);
      if (!pending) return;
      this.pending.delete(message.id);
      clearTimeout(pending.timer);
      if (message.error) pending.reject(new Error(message.error.message ?? 'CDP command failed.'));
      else pending.resolve(message.result ?? {});
      return;
    }
    if (message.method) {
      for (const listener of this.listeners.get(message.method) ?? []) {
        Promise.resolve(listener(message.params ?? {})).catch(() => {});
      }
    }
  }

  send(method, params = {}, timeoutMs = 15_000) {
    if (!this.socket || this.socket.readyState !== WebSocket.OPEN) {
      return Promise.reject(new Error('CDP WebSocket is not open.'));
    }
    const id = this.nextId++;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error(`CDP command timed out: ${method}`));
      }, timeoutMs);
      this.pending.set(id, { resolve, reject, timer });
      this.socket.send(JSON.stringify({ id, method, params }));
    });
  }

  on(method, listener) {
    const listeners = this.listeners.get(method) ?? [];
    listeners.push(listener);
    this.listeners.set(method, listeners);
  }

  waitFor(method, timeoutMs) {
    return new Promise((resolve, reject) => {
      const listener = (params) => {
        clearTimeout(timer);
        const listeners = this.listeners.get(method) ?? [];
        this.listeners.set(method, listeners.filter((entry) => entry !== listener));
        resolve(params);
      };
      const timer = setTimeout(() => {
        const listeners = this.listeners.get(method) ?? [];
        this.listeners.set(method, listeners.filter((entry) => entry !== listener));
        reject(new Error(`Timed out waiting for ${method}.`));
      }, timeoutMs);
      this.on(method, listener);
    });
  }

  close() {
    try { this.socket?.close(); } catch { /* Nothing else owns this socket. */ }
  }
}

async function readDevToolsEndpoint(profile, child, timeoutMs) {
  const activePort = path.join(profile, 'DevToolsActivePort');
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (child.exitCode !== null) throw new Error('The dedicated browser exited during startup.');
    try {
      const [portLine, pathLine] = (await readFile(activePort, 'utf8')).trim().split(/\r?\n/);
      const port = Number(portLine);
      if (Number.isInteger(port) && pathLine?.startsWith('/devtools/browser/')) {
        return { port, browserSocket: `ws://127.0.0.1:${port}${pathLine}` };
      }
    } catch {
      // Edge writes the file only after its DevTools endpoint is accepting connections.
    }
    await sleep(100);
  }
  throw new Error('The dedicated browser DevTools endpoint did not become ready.');
}

async function waitForPage(port, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    try {
      const response = await fetch(`http://127.0.0.1:${port}/json/list`);
      const targets = await response.json();
      const page = targets.find((target) => target.type === 'page' && target.webSocketDebuggerUrl);
      if (page) return page.webSocketDebuggerUrl;
    } catch {
      // The page target can appear shortly after the browser endpoint.
    }
    await sleep(100);
  }
  throw new Error('The dedicated browser did not expose a page target.');
}

async function navigate(page, url, timeoutMs) {
  const load = page.waitFor('Page.loadEventFired', timeoutMs).catch(() => null);
  const result = await page.send('Page.navigate', { url }, timeoutMs);
  if (result.errorText) throw new Error(`Navigation failed: ${result.errorText}`);
  await load;
  const deadline = Date.now() + Math.min(timeoutMs, 5000);
  while (Date.now() < deadline) {
    const state = await page.send('Runtime.evaluate', {
      expression: 'document.readyState', returnByValue: true,
    }, 2000).catch(() => null);
    if (state?.result?.value === 'complete' || state?.result?.value === 'interactive') break;
    await sleep(75);
  }
}

async function evaluateValue(page, expression, timeoutMs = 5000) {
  const response = await page.send('Runtime.evaluate', {
    expression,
    returnByValue: true,
    awaitPromise: true,
  }, timeoutMs);
  if (response.exceptionDetails) throw new Error('The browser page evaluation failed.');
  return response.result?.value;
}

async function showCursor(page, x, y, label) {
  const safeLabel = JSON.stringify(String(label).slice(0, 80));
  await evaluateValue(page, `(() => {
    let cursor = document.getElementById('__revia_visible_cursor');
    if (!cursor) {
      cursor = document.createElement('div');
      cursor.id = '__revia_visible_cursor';
      Object.assign(cursor.style, {
        position: 'fixed', zIndex: '2147483647', pointerEvents: 'none',
        width: '18px', height: '18px', borderRadius: '50%',
        border: '3px solid #69e2c4', boxShadow: '0 0 0 5px rgba(105,226,196,.22)',
        transition: 'left 180ms ease, top 180ms ease',
      });
      document.documentElement.appendChild(cursor);
    }
    cursor.style.left = ${Math.round(x)} + 'px';
    cursor.style.top = ${Math.round(y)} + 'px';
    cursor.title = ${safeLabel};
    return true;
  })()`).catch(() => null);
  await page.send('Input.dispatchMouseEvent', { type: 'mouseMoved', x, y }).catch(() => null);
}

async function visiblyEnterSearch(page, query, timeoutMs) {
  await navigate(page, SEARCH_HOME, timeoutMs);
  const input = await evaluateValue(page, `(() => {
    const element = document.querySelector('input[name="q"], input[type="search"], textarea[name="q"]');
    if (!element) return null;
    element.scrollIntoView({block:'center'});
    const rect = element.getBoundingClientRect();
    element.style.outline = '3px solid #69e2c4';
    element.style.outlineOffset = '3px';
    return {x: rect.left + rect.width / 2, y: rect.top + rect.height / 2};
  })()`);
  if (!input) return false;
  await showCursor(page, input.x, input.y, 'Revia search');
  await page.send('Input.dispatchMouseEvent', { type: 'mousePressed', x: input.x, y: input.y, button: 'left', clickCount: 1 });
  await page.send('Input.dispatchMouseEvent', { type: 'mouseReleased', x: input.x, y: input.y, button: 'left', clickCount: 1 });
  const visibleCharacters = Math.min(query.length, 120);
  const cadence = Math.max(8, Math.min(35, Math.floor(1400 / Math.max(1, visibleCharacters))));
  for (let index = 0; index < visibleCharacters; index += 1) {
    await page.send('Input.insertText', { text: query[index] });
    await sleep(cadence);
  }
  if (visibleCharacters < query.length) await page.send('Input.insertText', { text: query.slice(visibleCharacters) });
  const load = page.waitFor('Page.loadEventFired', timeoutMs).catch(() => null);
  await page.send('Input.dispatchKeyEvent', {
    type: 'keyDown', key: 'Enter', code: 'Enter', windowsVirtualKeyCode: 13,
  });
  await page.send('Input.dispatchKeyEvent', {
    type: 'keyUp', key: 'Enter', code: 'Enter', windowsVirtualKeyCode: 13,
  });
  await load;
  return true;
}

async function searchResults(page) {
  return await evaluateValue(page, `(() => {
    const selectors = ['a[data-testid="result-title-a"]', 'article h2 a', 'a.result__a'];
    const anchors = Array.from(document.querySelectorAll(selectors.join(',')));
    return anchors.slice(0, 30).map((anchor, index) => ({
      href: anchor.href,
      text: (anchor.innerText || anchor.textContent || '').trim(),
      index,
    })).filter((entry) => entry.href && entry.text);
  })()`) ?? [];
}

async function highlightResult(page, index, label) {
  const position = await evaluateValue(page, `(() => {
    const selectors = ['a[data-testid="result-title-a"]', 'article h2 a', 'a.result__a'];
    const anchor = Array.from(document.querySelectorAll(selectors.join(',')))[${Math.max(0, index)}];
    if (!anchor) return null;
    anchor.scrollIntoView({block:'center', behavior:'smooth'});
    anchor.style.outline = '3px solid #69e2c4';
    anchor.style.outlineOffset = '4px';
    const rect = anchor.getBoundingClientRect();
    return {x: rect.left + Math.min(24, rect.width / 2), y: rect.top + rect.height / 2};
  })()`).catch(() => null);
  if (position) await showCursor(page, position.x, position.y, label);
}

function normalizeText(value) {
  return String(value ?? '')
    .replace(/\r/g, '')
    .replace(/[\t ]+/g, ' ')
    .replace(/\n[\t ]+/g, '\n')
    .replace(/\n{3,}/g, '\n\n')
    .trim();
}

function utf8Prefix(value, maxBytes) {
  if (Buffer.byteLength(value, 'utf8') <= maxBytes) return value;
  const bytes = Buffer.from(value, 'utf8').subarray(0, maxBytes);
  return bytes.toString('utf8').replace(/\uFFFD+$/g, '');
}

class BrowserRuntime {
  constructor(settings) {
    this.settings = settings;
    this.child = null;
    this.browser = null;
    this.page = null;
    this.stopping = false;
  }

  async start() {
    const executable = await firstExisting([
      'C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe',
      'C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe',
      'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe',
      'C:\\Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe',
      path.join(process.env.LOCALAPPDATA ?? '', 'Google', 'Chrome', 'Application', 'chrome.exe'),
    ]);
    if (!executable) throw new Error('Microsoft Edge or Google Chrome was not found.');

    await rm(path.join(this.settings.profile, 'DevToolsActivePort'), { force: true }).catch(() => {});
    this.child = spawn(executable, [
      `--user-data-dir=${this.settings.profile}`,
      '--remote-debugging-address=127.0.0.1',
      '--remote-debugging-port=0',
      // Current Windows Edge builds can relaunch themselves to shed an inherited
      // compatibility layer. The launcher then exits successfully while the real
      // browser escapes the worker's job object, which looks like a crash and also
      // defeats deterministic shutdown. Edge documents this internal guard on the
      // relaunched process itself; passing it up front keeps the owned process stable.
      // Chromium-based browsers ignore unknown switches, so Chrome remains supported.
      '--edge-skip-compat-layer-relaunch',
      '--no-first-run',
      '--no-default-browser-check',
      '--disable-background-mode',
      '--disable-component-update',
      '--disable-features=msEdgeFirstRunExperience,msEdgeDefaultBrowserPrompt',
      '--new-window',
      'about:blank',
    ], { stdio: ['ignore', 'ignore', 'pipe'], windowsHide: false });
    this.child.stderr?.on('data', (chunk) => process.stderr.write(`[browser] ${String(chunk).slice(0, 2000)}`));
    this.child.once('exit', (code) => {
      if (!this.stopping) {
        process.stderr.write(`Dedicated browser exited unexpectedly (${code ?? 'unknown'}).\n`);
        setImmediate(() => process.exit(1));
      }
    });

    const endpoint = await readDevToolsEndpoint(this.settings.profile, this.child, 15_000);
    this.browser = new CdpClient(endpoint.browserSocket);
    await this.browser.connect();
    await this.browser.send('Browser.setDownloadBehavior', { behavior: 'deny', eventsEnabled: true });
    const pageSocket = await waitForPage(endpoint.port, 10_000);
    this.page = new CdpClient(pageSocket);
    await this.page.connect();
    await this.page.send('Page.enable');
    await this.page.send('Runtime.enable');
    await this.page.send('Network.enable');
    await this.page.send('Network.setBypassServiceWorker', { bypass: true });
    await this.page.send('Network.setBlockedURLs', { urls: ['ws://*', 'wss://*'] });
    this.page.on('Page.javascriptDialogOpening', async () => {
      await this.page.send('Page.handleJavaScriptDialog', { accept: false }).catch(() => {});
    });
    this.page.on('Fetch.requestPaused', async (event) => {
      const document = event.resourceType === 'Document';
      const method = String(event.request?.method ?? '').toUpperCase();
      const allowedMethod = method === 'GET' || method === 'HEAD';
      const allowed = allowedMethod
        ? await allowedPublicUrl(event.request?.url ?? '', { httpsOnly: document })
        : null;
      await this.page.send(allowed ? 'Fetch.continueRequest' : 'Fetch.failRequest', allowed
        ? { requestId: event.requestId }
        : { requestId: event.requestId, errorReason: 'BlockedByClient' }).catch(() => {});
    });
    await this.page.send('Fetch.enable', {
      patterns: [{ urlPattern: '*', requestStage: 'Request' }],
    });
  }

  async search(request) {
    const query = typeof request.query === 'string' ? request.query.trim() : '';
    if (!query || Buffer.byteLength(query, 'utf8') > 1024) {
      throw new Error('Visible browser search requires a query no longer than 1024 bytes.');
    }
    const maxPages = Math.max(1, Math.min(
      this.settings.maxPages, 5, Math.trunc(Number(request.max_results)) || 1));
    const maxBytes = Math.max(4096, Math.min(
      2 * 1024 * 1024, Math.trunc(Number(request.max_response_bytes)) || 262144));
    const timeoutMs = Math.max(5000, Math.min(
      120000, Math.trunc(Number(request.timeout_ms)) || 30000));
    const stepDelayMs = Math.max(0, Math.min(
      this.settings.stepDelayMs, 3000, Math.trunc(Number(request.step_delay_ms)) || 0));
    const deadline = Date.now() + timeoutMs;

    const typed = await visiblyEnterSearch(this.page, query, Math.min(15_000, timeoutMs));
    if (!typed) {
      await navigate(this.page, `${SEARCH_HOME}?q=${encodeURIComponent(query)}`, Math.min(15_000, timeoutMs));
    }
    await sleep(stepDelayMs);
    let rawResults = await searchResults(this.page);
    if (typed && rawResults.length === 0) {
      // DuckDuckGo occasionally completes a typed search through client-side
      // navigation without emitting the load event we waited for. Reloading the same
      // public GET URL is still visible and auditable, and makes repeated searches as
      // reliable as the first one instead of immediately reporting an empty page.
      await navigate(
        this.page,
        `${SEARCH_HOME}?q=${encodeURIComponent(query)}`,
        Math.max(2000, Math.min(15_000, deadline - Date.now())),
      );
      await sleep(stepDelayMs);
      rawResults = await searchResults(this.page);
    }
    const candidates = [];
    const seen = new Set();
    for (const raw of rawResults) {
      if (candidates.length >= maxPages) break;
      const unwrapped = unwrapDuckDuckGoUrl(raw.href);
      const allowed = await allowedPublicUrl(unwrapped, { httpsOnly: true });
      if (!allowed || seen.has(allowed.href)) continue;
      seen.add(allowed.href);
      candidates.push({ url: allowed.href, text: raw.text, index: raw.index });
    }
    if (candidates.length === 0) {
      return { succeeded: false, message: 'The visible DuckDuckGo search returned no public HTTPS result pages.', content: '', entries: [] };
    }

    let content = '';
    const entries = [];
    for (let index = 0; index < candidates.length && Date.now() < deadline; index += 1) {
      const candidate = candidates[index];
      if (index === 0) {
        await highlightResult(this.page, candidate.index, `Revia selected: ${candidate.text}`);
        await sleep(stepDelayMs);
      }
      await navigate(this.page, candidate.url, Math.max(2000, Math.min(15_000, deadline - Date.now())));
      await sleep(stepDelayMs);
      const pageData = await evaluateValue(this.page, `(() => ({
        url: location.href,
        title: document.title,
        text: (document.querySelector('main, article, [role="main"]') || document.body)?.innerText || ''
      }))()`, 7000);
      const finalUrl = await allowedPublicUrl(pageData?.url ?? '', { httpsOnly: true });
      const readable = normalizeText(pageData?.text);
      if (!finalUrl || readable.length < 80 || seen.has(`visited:${finalUrl.href}`)) continue;
      seen.add(`visited:${finalUrl.href}`);
      const section = `${normalizeText(pageData.title) || candidate.text}\nURL: ${finalUrl.href}\n${readable}\nSource: ${finalUrl.href}`;
      const separator = content ? '\n\n' : '';
      const remaining = maxBytes - Buffer.byteLength(content + separator, 'utf8');
      if (remaining <= 64) break;
      content += separator + utf8Prefix(section, remaining);
      entries.push(finalUrl.href);
    }
    return entries.length > 0
      ? {
          succeeded: true,
          message: `Visible browser visited ${entries.length} public HTTPS ${entries.length === 1 ? 'source' : 'sources'}.`,
          content,
          entries,
        }
      : {
          succeeded: false,
          message: 'The visible browser found results but could not extract readable public HTTPS pages within its limits.',
          content: '',
          entries: [],
        };
  }

  async stop() {
    this.stopping = true;
    this.page?.close();
    this.browser?.close();
    if (this.child && this.child.exitCode === null) {
      this.child.kill('SIGTERM');
      await Promise.race([
        new Promise((resolve) => this.child.once('exit', resolve)),
        sleep(1500),
      ]);
      if (this.child.exitCode === null) this.child.kill('SIGKILL');
    }
  }
}

function authorized(request, token) {
  const header = request.headers.authorization ?? '';
  const expected = Buffer.from(`Bearer ${token}`, 'utf8');
  const received = Buffer.from(String(header), 'utf8');
  return expected.length === received.length && timingSafeEqual(expected, received);
}

function loopbackPeer(request) {
  const address = request.socket.remoteAddress ?? '';
  return address === '127.0.0.1' || address === '::1' || address === '::ffff:127.0.0.1';
}

function jsonResponse(response, status, value) {
  const body = Buffer.from(JSON.stringify(value), 'utf8');
  response.writeHead(status, {
    'Content-Type': 'application/json; charset=utf-8',
    'Content-Length': body.length,
    'Cache-Control': 'no-store',
    'X-Content-Type-Options': 'nosniff',
  });
  response.end(body);
}

async function requestJson(request) {
  const chunks = [];
  let length = 0;
  for await (const chunk of request) {
    length += chunk.length;
    if (length > MAX_REQUEST_BODY) throw new Error('Request body exceeded its byte limit.');
    chunks.push(chunk);
  }
  return JSON.parse(Buffer.concat(chunks).toString('utf8') || '{}');
}

async function main() {
  const settings = parseArguments(process.argv.slice(2));
  const runtime = new BrowserRuntime(settings);
  await runtime.start();
  let queue = Promise.resolve();
  let server;
  let stopping = false;
  const shutdown = async () => {
    if (stopping) return;
    stopping = true;
    await new Promise((resolve) => server?.close(resolve));
    await runtime.stop();
  };

  server = http.createServer(async (request, response) => {
    if (!loopbackPeer(request) || !authorized(request, settings.token)) {
      jsonResponse(response, 401, { succeeded: false, message: 'Unauthorized.' });
      return;
    }
    if (request.method === 'GET' && request.url === '/health') {
      jsonResponse(response, 200, { ready: true, message: 'Visible browser is ready.' });
      return;
    }
    if (request.method === 'POST' && request.url === '/shutdown') {
      jsonResponse(response, 200, { succeeded: true, message: 'Stopping.' });
      setImmediate(() => shutdown().finally(() => process.exit(0)));
      return;
    }
    if (request.method === 'POST' && request.url === '/search') {
      try {
        const body = await requestJson(request);
        const operation = queue.then(() => runtime.search(body));
        queue = operation.catch(() => {});
        jsonResponse(response, 200, await operation);
      } catch (error) {
        jsonResponse(response, 500, {
          succeeded: false,
          message: error instanceof Error ? error.message : 'Visible browser search failed.',
          content: '',
          entries: [],
        });
      }
      return;
    }
    jsonResponse(response, 404, { succeeded: false, message: 'Not found.' });
  });
  server.requestTimeout = 125_000;
  server.headersTimeout = 10_000;
  server.listen(settings.port, settings.host);
  await new Promise((resolve, reject) => {
    server.once('listening', resolve);
    server.once('error', reject);
  });
  process.stdout.write(`Visible browser worker listening on ${settings.host}:${settings.port}.\n`);
  process.on('SIGINT', () => shutdown().finally(() => process.exit(0)));
  process.on('SIGTERM', () => shutdown().finally(() => process.exit(0)));
}

const isMain = process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url);
if (isMain) {
  main().catch((error) => {
    process.stderr.write(`${error instanceof Error ? error.stack ?? error.message : String(error)}\n`);
    process.exit(1);
  });
}
