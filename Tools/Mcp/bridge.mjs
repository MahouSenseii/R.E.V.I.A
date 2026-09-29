// The bridge from Revia to MCP servers that speak stdio.
//
//   node bridge.mjs bridge.config.json
//
// Revia only ever connects to an HTTP endpoint named in a manifest under Config/Skills;
// she never starts a process. Most MCP servers speak JSON-RPC over stdio, so the owner
// runs this bridge, which starts each configured server on first use and exposes it at
// http://127.0.0.1:<port>/<id>: a POST carrying one JSON-RPC message is written to the
// server's stdin, and the line it answers with the same id is the HTTP response. A
// notification (no id) is written and acknowledged with 202. Only loopback peers are
// served, and a token in the config is required as a bearer when set. Nothing here
// changes what a tool does or says; pinning and policy live on Revia's side.
import { spawn } from 'node:child_process';
import { readFile } from 'node:fs/promises';
import http from 'node:http';
import path from 'node:path';

const REQUEST_TIMEOUT_MS = 60_000;
const MAX_BODY_BYTES = 1024 * 1024;

export function validateConfig(config) {
  if (!config || typeof config !== 'object') throw new Error('The bridge config must be a JSON object.');
  const port = Number(config.port ?? 8760);
  if (!Number.isInteger(port) || port < 0 || port > 65535) throw new Error('port must be an integer.');
  const servers = config.servers && typeof config.servers === 'object' ? config.servers : {};
  for (const [id, server] of Object.entries(servers)) {
    if (!/^[A-Za-z0-9_.-]{1,64}$/.test(id)) throw new Error(`"${id}" is not a server id (letters, digits, '_', '-', '.').`);
    if (!server || typeof server.command !== 'string' || !server.command) throw new Error(`Server "${id}" needs a command.`);
    if (server.args !== undefined && !Array.isArray(server.args)) throw new Error(`Server "${id}": args must be an array.`);
    if (server.env !== undefined && (typeof server.env !== 'object' || server.env === null)) throw new Error(`Server "${id}": env must be an object.`);
  }
  return { port, token: typeof config.token === 'string' ? config.token : '', servers };
}

// ${NAME} in an arg or env value is taken from the bridge's own environment, so a
// token can stay out of the config file.
function expand(value) {
  return String(value).replace(/\$\{([A-Z0-9_]+)\}/g, (_, name) => process.env[name] ?? '');
}

class StdioServer {
  constructor(id, spec) {
    this.id = id;
    this.spec = spec;
    this.child = null;
    this.pending = new Map();
    this.buffer = '';
  }

  start() {
    if (this.child && this.child.exitCode === null) return;
    const env = { ...process.env };
    for (const [name, value] of Object.entries(this.spec.env ?? {})) env[name] = expand(value);
    this.child = spawn(this.spec.command, (this.spec.args ?? []).map(expand), {
      stdio: ['pipe', 'pipe', 'pipe'], env, windowsHide: true, shell: process.platform === 'win32',
    });
    this.buffer = '';
    this.child.stdout.setEncoding('utf8');
    this.child.stdout.on('data', chunk => this.onData(chunk));
    this.child.stderr.setEncoding('utf8');
    this.child.stderr.on('data', chunk => process.stderr.write(`[${this.id}] ${String(chunk).slice(0, 2000)}`));
    this.child.once('exit', code => {
      for (const { reject } of this.pending.values()) reject(new Error(`The ${this.id} server exited (${code ?? 'signal'}).`));
      this.pending.clear();
      process.stderr.write(`[bridge] ${this.id} exited (${code ?? 'signal'}); it restarts on the next request.\n`);
    });
  }

  onData(chunk) {
    this.buffer += chunk;
    let newline;
    while ((newline = this.buffer.indexOf('\n')) >= 0) {
      const line = this.buffer.slice(0, newline).trim();
      this.buffer = this.buffer.slice(newline + 1);
      if (!line) continue;
      let message;
      try { message = JSON.parse(line); } catch { continue; }
      if (message && Object.hasOwn(message, 'id') && this.pending.has(String(message.id))) {
        const { resolve } = this.pending.get(String(message.id));
        this.pending.delete(String(message.id));
        resolve(message);
      }
    }
  }

  send(message) {
    this.start();
    const line = JSON.stringify(message) + '\n';
    if (!Object.hasOwn(message, 'id') || message.id === null) {
      this.child.stdin.write(line);
      return Promise.resolve(null);
    }
    return new Promise((resolve, reject) => {
      const key = String(message.id);
      const timer = setTimeout(() => {
        this.pending.delete(key);
        reject(new Error(`The ${this.id} server did not answer within ${REQUEST_TIMEOUT_MS / 1000} s.`));
      }, REQUEST_TIMEOUT_MS);
      this.pending.set(key, {
        resolve: value => { clearTimeout(timer); resolve(value); },
        reject: error => { clearTimeout(timer); reject(error); },
      });
      this.child.stdin.write(line, error => {
        if (error) { clearTimeout(timer); this.pending.delete(key); reject(error); }
      });
    });
  }

  stop() {
    if (this.child && this.child.exitCode === null) this.child.kill();
  }
}

function loopbackPeer(request) {
  const address = request.socket.remoteAddress ?? '';
  return address === '127.0.0.1' || address === '::1' || address === '::ffff:127.0.0.1';
}

async function readBody(request) {
  const chunks = [];
  let size = 0;
  for await (const chunk of request) {
    size += chunk.length;
    if (size > MAX_BODY_BYTES) throw new Error('The request body is too large.');
    chunks.push(chunk);
  }
  return Buffer.concat(chunks).toString('utf8');
}

function answer(response, status, body) {
  const text = body === undefined ? '' : JSON.stringify(body);
  response.writeHead(status, { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(text) });
  response.end(text);
}

export function createBridge(rawConfig) {
  const config = validateConfig(rawConfig);
  const servers = new Map(Object.entries(config.servers).map(([id, spec]) => [id, new StdioServer(id, spec)]));
  const server = http.createServer(async (request, response) => {
    if (!loopbackPeer(request)) return answer(response, 403, { error: 'loopback only' });
    if (config.token && request.headers.authorization !== `Bearer ${config.token}`) return answer(response, 401, { error: 'unauthorized' });
    if (request.method === 'GET' && request.url === '/health') {
      return answer(response, 200, { status: 'ok', servers: [...servers.keys()] });
    }
    const id = (request.url ?? '').replace(/^\/+/, '').split(/[/?]/)[0];
    const target = servers.get(id);
    if (!target) return answer(response, 404, { error: `no server "${id}"` });
    if (request.method !== 'POST') return answer(response, 405, { error: 'POST one JSON-RPC message' });
    let message;
    try { message = JSON.parse(await readBody(request)); } catch (error) { return answer(response, 400, { error: error.message }); }
    if (!message || typeof message !== 'object' || message.jsonrpc !== '2.0' || typeof message.method !== 'string') {
      return answer(response, 400, { error: 'not a JSON-RPC 2.0 message' });
    }
    try {
      const reply = await target.send(message);
      if (reply === null) return answer(response, 202);
      return answer(response, 200, reply);
    } catch (error) {
      return answer(response, 502, { jsonrpc: '2.0', id: message.id ?? null, error: { code: -32000, message: error.message } });
    }
  });
  return {
    server,
    listen: port => new Promise(resolve => server.listen(port ?? config.port, '127.0.0.1', () => resolve(server.address().port))),
    close: () => { for (const target of servers.values()) target.stop(); server.close(); },
  };
}

if (process.argv[1] && path.resolve(process.argv[1]) === new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1')) {
  const configPath = process.argv[2] ?? new URL('./bridge.config.json', import.meta.url);
  const bridge = createBridge(JSON.parse(await readFile(configPath, 'utf8')));
  const port = await bridge.listen();
  console.log(`MCP bridge listening on http://127.0.0.1:${port}/<server id>; servers start on first use.`);
  const stop = () => { bridge.close(); console.log('MCP bridge stopped.'); };
  process.once('SIGINT', stop);
  process.once('SIGTERM', stop);
}
