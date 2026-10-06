import { spawn } from 'node:child_process';
import { lookup } from 'node:dns/promises';
import { existsSync } from 'node:fs';
import { mkdir } from 'node:fs/promises';
import { createHash, randomUUID } from 'node:crypto';
import net from 'node:net';
import path from 'node:path';
import { pathToFileURL } from 'node:url';

const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const bytes = value => Buffer.byteLength(value ?? '', 'utf8');
function bounded(value, maximum) {
  let result = String(value ?? '');
  if (bytes(result) > maximum) result = Buffer.from(result).subarray(0, maximum).toString('utf8').replace(/\uFFFD$/, '');
  return result;
}
export function publicAddress(address) {
  if (net.isIP(address) === 4) {
    const [a, b] = address.split('.').map(Number);
    return !(a === 0 || a === 10 || a === 127 || a >= 224 || (a === 169 && b === 254) ||
      (a === 172 && b >= 16 && b <= 31) || (a === 192 && (b === 168 || b === 0)) ||
      (a === 100 && b >= 64 && b <= 127) || (a === 198 && (b === 18 || b === 19)));
  }
  if (net.isIP(address) === 6) return /^2[0-9a-f]{3}:/i.test(address) && !/^2001:(?:db8|0|10|20):/i.test(address);
  return false;
}
const loopbackHost = host => ['127.0.0.1', '[::1]', '::1', 'localhost'].includes(host.toLowerCase());
export function allowedOrigin(raw, grant) {
  if (!grant?.enabled || typeof raw !== 'string' || bytes(raw) > 4096 || /[\s\\]/.test(raw)) return false;
  try {
    const url = new URL(raw);
    if (url.username || url.password || !['http:', 'https:'].includes(url.protocol)) return false;
    if (!Array.isArray(grant.approvedOrigins) || !grant.approvedOrigins.includes(url.origin)) return false;
    if (loopbackHost(url.hostname)) return !!grant.allowLoopback;
    if (url.protocol !== 'https:') return false;
    const host = url.hostname.replace(/^\[|\]$/g, '');
    return !net.isIP(host) || publicAddress(host);
  } catch { return false; }
}
export function validateOperation(request, current) {
  const fields = ['operation', 'url', 'session', 'generation', 'element', 'value', 'grant'];
  if (!request || Object.keys(request).some(key => !fields.includes(key))) return 'Unknown browser request field.';
  if (!['navigate', 'observe', 'click', 'fill'].includes(request.operation)) return 'Unknown browser operation.';
  const grant = request.grant;
  if (!allowedOrigin(request.url, grant)) return 'The URL origin is not admitted.';
  if (request.operation === 'navigate' && !grant.navigate) return 'Browser navigation is disabled.';
  if (['click', 'fill'].includes(request.operation)) {
    if (!grant.interact) return 'Browser interaction is disabled.';
    if (request.session !== current.session || request.generation !== current.generation ||
      typeof request.element !== 'string' || !request.element) return 'The browser target observation is stale.';
    if (typeof request.value !== 'string' || bytes(request.value) > grant.maxValueBytes || request.value.includes('\0'))
      return 'Browser input exceeds its content bound.';
  }
  if (!Number.isInteger(grant.timeoutMs) || grant.timeoutMs < 100 || grant.timeoutMs > 60000 ||
    !Number.isInteger(grant.maxElements) || grant.maxElements < 1 || grant.maxElements > 200 ||
    !Number.isInteger(grant.maxTextBytes) || grant.maxTextBytes < 256 || grant.maxTextBytes > 65536 ||
    !Number.isInteger(grant.maxValueBytes) || grant.maxValueBytes < 1 || grant.maxValueBytes > 16384)
    return 'Invalid browser bounds.';
  return '';
}

class CdpPipe {
  constructor(child) {
    this.child = child; this.next = 1; this.pending = new Map(); this.listeners = new Map(); this.buffer = '';
    child.stdio[4].setEncoding('utf8');
    child.stdio[4].on('data', chunk => {
      this.buffer += chunk;
      if (this.buffer.length > 4 * 1024 * 1024) { child.kill(); return; }
      for (;;) {
        const end = this.buffer.indexOf('\0'); if (end < 0) break;
        let message; try { message = JSON.parse(this.buffer.slice(0, end)); } catch { child.kill(); return; }
        this.buffer = this.buffer.slice(end + 1);
        if (message.id) {
          const pending = this.pending.get(message.id); if (!pending) continue;
          clearTimeout(pending.timer); this.pending.delete(message.id);
          if (message.error) pending.reject(new Error(message.error.message)); else pending.resolve(message.result ?? {});
        } else for (const listener of this.listeners.get(message.method) ?? [])
          Promise.resolve(listener(message.params ?? {}, message.sessionId)).catch(() => {});
      }
    });
    child.once('exit', () => { for (const value of this.pending.values()) { clearTimeout(value.timer); value.reject(new Error('Owned browser exited.')); } this.pending.clear(); });
  }
  on(method, callback) { this.listeners.set(method, [...(this.listeners.get(method) ?? []), callback]); }
  send(method, params = {}, sessionId, timeout = 5000) {
    const id = this.next++;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => { this.pending.delete(id); reject(new Error('Browser operation timed out.')); }, timeout);
      this.pending.set(id, { resolve, reject, timer });
      this.child.stdio[3].write(`${JSON.stringify({ id, method, params, ...(sessionId ? { sessionId } : {}) })}\0`);
    });
  }
}

export class InteractiveBrowser {
  constructor(profile, executable) {
    this.profile = profile; this.executable = executable; this.session = randomUUID(); this.generation = 0;
    this.grant = null; this.child = null; this.cdp = null; this.page = null; this.context = null; this.last = null;
  }
  async start() {
    const candidates = [this.executable, 'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',
      'C:/Program Files/Microsoft/Edge/Application/msedge.exe', 'C:/Program Files/Google/Chrome/Application/chrome.exe'];
    const executable = candidates.find(candidate => candidate && existsSync(candidate));
    if (!executable) throw new Error('No installed Chromium browser was found.');
    await mkdir(this.profile, { recursive: true });
    this.child = spawn(executable, [`--user-data-dir=${this.profile}`, '--remote-debugging-pipe',
      '--edge-skip-compat-layer-relaunch', '--no-first-run', '--no-default-browser-check', '--disable-background-mode',
      '--disable-component-update', '--disable-sync', '--disable-extensions', '--disable-features=msEdgeFirstRunExperience',
      '--disable-quic', '--force-webrtc-ip-handling-policy=disable_non_proxied_udp', '--new-window', 'about:blank'],
    { stdio: ['ignore', 'ignore', 'ignore', 'pipe', 'pipe'], windowsHide: true });
    this.cdp = new CdpPipe(this.child);
    await this.cdp.send('Browser.setDownloadBehavior', { behavior: 'deny' });
    const { targetInfos } = await this.cdp.send('Target.getTargets');
    const target = targetInfos.find(value => value.type === 'page');
    if (!target) throw new Error('The owned browser did not create a page.');
    this.target = target.targetId;
    const attached = await this.cdp.send('Target.attachToTarget', { targetId: this.target, flatten: true });
    this.page = attached.sessionId;
    this.cdp.on('Target.attachedToTarget', info => {
      if (info.targetInfo.targetId !== this.target) return this.cdp.send('Target.closeTarget', { targetId: info.targetInfo.targetId });
    });
    await this.cdp.send('Target.setAutoAttach', { autoAttach: true, waitForDebuggerOnStart: true, flatten: true });
    this.cdp.on('Target.targetCreated', info => { if (info.targetInfo.type === 'page' && info.targetInfo.targetId !== this.target)
      return this.cdp.send('Target.closeTarget', { targetId: info.targetInfo.targetId }); });
    await this.cdp.send('Target.setDiscoverTargets', { discover: true });
    this.cdp.on('Page.frameNavigated', (_, session) => { if (session === this.page) this.context = null; });
    this.cdp.on('Page.javascriptDialogOpening', () => this.send('Page.handleJavaScriptDialog', { accept: false }));
    this.cdp.on('Fetch.requestPaused', async request => {
      const admitted = await this.allowedNetwork(request.request.url);
      await this.send(admitted ? 'Fetch.continueRequest' : 'Fetch.failRequest', admitted ? { requestId: request.requestId }
        : { requestId: request.requestId, errorReason: 'BlockedByClient' });
    });
    await this.send('Page.enable'); await this.send('Runtime.enable'); await this.send('Network.enable');
    await this.send('Network.setBypassServiceWorker', { bypass: true });
    await this.send('Network.setBlockedURLs', { urls: ['ws://*', 'wss://*'] });
    await this.send('Fetch.enable', { patterns: [{ urlPattern: '*', requestStage: 'Request' }] });
  }
  send(method, params = {}) { return this.cdp.send(method, params, this.page, Math.min(this.grant?.timeoutMs ?? 5000, 15000)); }
  async allowedNetwork(raw) {
    if (!allowedOrigin(raw, this.grant)) return false;
    const host = new URL(raw).hostname.replace(/^\[|\]$/g, '');
    if (loopbackHost(host)) return !!this.grant.allowLoopback;
    try {
      const addresses = await lookup(host, { all: true });
      return addresses.length > 0 && addresses.every(value => publicAddress(value.address));
    } catch { return false; }
  }
  async evaluate(expression) {
    if (!this.context) {
      const { frameTree } = await this.send('Page.getFrameTree');
      const world = await this.send('Page.createIsolatedWorld', { frameId: frameTree.frame.id, worldName: 'revia-interactive-owner' });
      this.context = world.executionContextId;
    }
    const response = await this.send('Runtime.evaluate', { expression, contextId: this.context, returnByValue: true, awaitPromise: true });
    if (response.exceptionDetails) throw new Error('The current page could not be observed.');
    return response.result?.value;
  }
  async observe() {
    const generation = ++this.generation;
    const observed = await this.evaluate(`(() => {
      const shown = e => { const r=e.getBoundingClientRect(); const s=getComputedStyle(e); return r.width>0 && r.height>0 && s.visibility!=='hidden' && s.display!=='none' && !e.closest('[hidden],[inert]'); };
      const allowed = e => !e.disabled && e.type!=='password' && e.type!=='file' && shown(e);
      const elements=Array.from(document.querySelectorAll('a[href],button,input,textarea,select,[role="button"],[contenteditable="true"]')).filter(allowed).slice(0,${this.grant.maxElements});
      const identities=globalThis.__reviaIdentities??={nodes:new WeakMap(),next:0,document};
      const map=new Map();
      const describe=e=>{
        let id=identities.nodes.get(e); if(!id){id='node-'+(++identities.next);identities.nodes.set(e,id);}
        const editable=['INPUT','TEXTAREA'].includes(e.tagName)&&!['checkbox','radio','button','submit','reset'].includes(e.type)||e.isContentEditable;
        const value=editable?String(e.isContentEditable?e.textContent:e.value):'';
        const valueAvailable=editable&&new TextEncoder().encode(value).length<=${this.grant.maxValueBytes};
        return {id,name:(e.getAttribute('aria-label')||e.labels?.[0]?.innerText||e.innerText||e.placeholder||e.name||e.id||'').slice(0,180),
          role:e.tagName.toLowerCase(),clickable:e.tagName!=='TEXTAREA',editable,valueAvailable,value:valueAvailable?value:''};};
      const rows=elements.map(e=>{const row=describe(e);map.set(row.id,e);return row;});
      const text=(document.body?.innerText||'').slice(0,${this.grant.maxTextBytes});
      const signature=JSON.stringify({url:location.href,text,rows,values:elements.map(e=>e.value??e.textContent)});
      globalThis.__reviaOwned={map,signature,document,shown,allowed,describe};
      return {url:location.href,title:document.title,text,elements:rows,signature};
    })()`);
    if (!allowedOrigin(observed.url, this.grant)) throw new Error('The current browser origin is no longer admitted.');
    const fingerprint = createHash('sha256').update(observed.signature).digest('hex');
    this.last = { session: this.session, generation, url: observed.url, title: bounded(observed.title, 1024),
      text: bounded(observed.text, this.grant.maxTextBytes), elements: observed.elements, fingerprint };
    this.signature = observed.signature;
    return this.last;
  }
  async operate(request) {
    const failure = validateOperation(request, this);
    if (failure) throw new Error(failure);
    this.grant = request.grant;
    if (!this.child) await this.start();
    let effect = false;
    try {
      if (request.operation === 'navigate') {
        if (!await this.allowedNetwork(request.url)) throw new Error('The navigation address is not admitted.');
        effect = true; this.context = null;
        const result = await this.send('Page.navigate', { url: request.url });
        if (result.errorText) throw new Error('Browser navigation failed.');
        const deadline = Date.now() + this.grant.timeoutMs;
        for (;;) {
          const ready = await this.evaluate('document.readyState').catch(() => 'loading');
          if (ready === 'complete' || ready === 'interactive') break;
          if (Date.now() >= deadline) throw new Error('Browser navigation timed out.');
          await sleep(40);
        }
      } else {
        const url = await this.evaluate('location.href');
        if ((request.operation !== 'observe' && url !== request.url) || !allowedOrigin(url, this.grant)) throw new Error('The page changed since the supplied observation.');
        if (request.operation !== 'observe') {
          effect = true;
          const result = await this.evaluate(`(() => {
            const owned=globalThis.__reviaOwned; const e=owned?.map.get(${JSON.stringify(request.element)});
            if (!e || !e.isConnected || owned.document!==document || !owned.allowed(e)) return 'stale';
            const elements=Array.from(document.querySelectorAll('a[href],button,input,textarea,select,[role="button"],[contenteditable="true"]')).filter(owned.allowed).slice(0,${this.grant.maxElements});
            const rows=elements.map(owned.describe);
            const signature=JSON.stringify({url:location.href,text:(document.body?.innerText||'').slice(0,${this.grant.maxTextBytes}),rows,values:elements.map(e=>e.value??e.textContent)});
            if(signature!==owned.signature) return 'stale';
            if(${JSON.stringify(request.operation)}==='fill') {
              if(!(['INPUT','TEXTAREA'].includes(e.tagName)||e.isContentEditable)) return 'not editable';
              e.focus(); const value=${JSON.stringify(request.value)};
              if(e.isContentEditable) e.textContent=value;
              else Object.getOwnPropertyDescriptor(e.tagName==='INPUT'?HTMLInputElement.prototype:HTMLTextAreaElement.prototype,'value').set.call(e,value);
              e.dispatchEvent(new Event('input',{bubbles:true}));e.dispatchEvent(new Event('change',{bubbles:true}));
            } else e.click();
            globalThis.__reviaOwned=null; return 'effect';
          })()`);
          if (result !== 'effect') { effect = false; throw new Error('The element or page changed; observe again before interacting.'); }
          effect = true;
          await sleep(60);
        }
      }
      return { succeeded: true, attempted: effect, message: 'Current browser state observed.', receipt: await this.observe() };
    } catch (error) {
      this.generation++; this.last = null;
      return { succeeded: false, attempted: effect, uncertainEffect: effect, message: String(error.message).slice(0,400) };
    }
  }
  async stop() {
    if (this.cdp) await this.cdp.send('Browser.close').catch(() => {});
    this.child?.kill(); this.child = null;
  }
}

async function main() {
  const args = process.argv.slice(2); const profile = args[args.indexOf('--profile') + 1];
  if (!profile || !path.isAbsolute(profile)) throw new Error('An owned absolute browser profile is required.');
  const runtime = new InteractiveBrowser(profile);
  let pending = ''; let chain = Promise.resolve();
  process.stdin.setEncoding('utf8');
  process.stdin.on('data', chunk => {
    pending += chunk;
    if (bytes(pending) > 131072) process.exit(2);
    for (;;) {
      const newline = pending.indexOf('\n'); if (newline < 0) break;
      const line = pending.slice(0, newline); pending = pending.slice(newline + 1);
      chain = chain.then(async () => {
        let result;
        try { result = await runtime.operate(JSON.parse(line)); }
        catch (error) { result = { succeeded: false, attempted: false, message: String(error.message).slice(0,400) }; }
        process.stdout.write(`${JSON.stringify(result)}\n`);
      });
    }
  });
  process.stdin.on('end', () => runtime.stop().finally(() => process.exit(0)));
}
if (process.argv[1] && import.meta.url === pathToFileURL(path.resolve(process.argv[1])).href)
  main().catch(error => { process.stderr.write(`${error.message}\n`); process.exit(1); });
