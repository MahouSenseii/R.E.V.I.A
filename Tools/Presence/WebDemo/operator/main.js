import { spawn } from 'node:child_process';
import { access } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { resolve } from 'node:path';
import net from 'node:net';
import { createBridge } from '../bridge/client.js';
import { loadSettings, nativeEnvironment, readStatus } from './config.js';

const directory = fileURLToPath(new URL('..', import.meta.url));
const repo = resolve(directory, '../../..');
const [action = 'help', ...args] = process.argv.slice(2);
const actions = ['start', 'revia', 'bridge', 'check', 'status-local', 'status-relay'];
let bridge;
let stopping = false;
async function stop() {
  stopping = true;
  await bridge?.stop();
  console.log('Connector stopped. Close Revia separately when finished.');
}
for (const signal of ['SIGINT', 'SIGTERM']) process.once(signal, stop);

async function portIsOpen(port) {
  return new Promise(resolve => {
    const socket = net.connect({ host: '127.0.0.1', port });
    const done = value => { socket.destroy(); resolve(value); };
    socket.setTimeout(1500, () => done(true));
    socket.once('connect', () => done(true));
    socket.once('error', () => done(false));
  });
}

try {
  if (action === 'help') {
    console.log('Web demo: start | revia | bridge | check | status-local | status-relay');
    console.log('Optional: --config-dir DIRECTORY, --exe PATH (native actions). Defaults: WebDemo and build/debug/ReviaDesktop.exe.');
  } else {
    if (!actions.includes(action)) throw Error('Unknown action');
    let configDir = directory, executable = resolve(repo, 'build/debug/ReviaDesktop.exe');
    for (let i = 0; i < args.length; i += 2) {
      if (!args[i + 1] || !['--config-dir', '--exe'].includes(args[i])) throw Error('Invalid argument');
      if (args[i] === '--config-dir') configDir = resolve(args[i + 1]);
      else executable = resolve(args[i + 1]);
    }
    const config = await loadSettings(configDir, !['revia', 'status-local'].includes(action));
    if (action === 'check') console.log('Native and connector configuration valid; no network connection made.');
    else if (action === 'status-local') console.log(`Native: ${await readStatus(config.localUrl + '/web/v1/status', config.token)}`);
    else if (action === 'status-relay') console.log(`Relay: ${await readStatus(config.relayOrigin + '/v1/status')}`);
    else {
      if (['start', 'revia'].includes(action)) {
        await access(executable);
        if (await portIsOpen(config.port)) throw Error('Local port already occupied; use bridge for an existing Revia');
        const desktop = spawn(executable, [], { cwd: repo, env: nativeEnvironment(config), windowsHide: true, detached: true, stdio: 'ignore' });
        await new Promise((resolve, reject) => { desktop.once('spawn', resolve); desktop.once('error', reject); });
        desktop.unref();
        console.log('Revia launched with web guest enabled. Waiting up to three minutes for native readiness.');
        const deadline = Date.now() + 180000;
        let ready = false;
        while (!stopping && Date.now() < deadline) {
          if (desktop.exitCode !== null) throw Error('Revia exited');
          let state;
          try { state = await readStatus(config.localUrl + '/web/v1/status', config.token); }
          catch { /* The listener may still be starting. */ }
          if (['online', 'busy'].includes(state)) { ready = true; console.log(`Native: ${state}`); break; }
          if (state === 'offline' || state === 'paused') throw Error('Native disabled or paused');
          await new Promise(resolve => setTimeout(resolve, 1000));
        }
        if (!ready && !stopping) throw Error('Native readiness timeout');
      }
      if (!stopping && ['start', 'bridge'].includes(action)) {
        const state = await readStatus(config.localUrl + '/web/v1/status', config.token);
        if (state === 'offline') throw Error('Native disabled');
        bridge = createBridge(config.bridge); bridge.start();
        console.log('Connector running in this terminal. Use status-relay to verify public readiness; Ctrl+C stops the connector.');
      }
    }
  }
} catch {
  await bridge?.stop();
  console.error('Web demo command failed. Check action/paths, Node 24, local configuration files, independent secrets, local port availability, native readiness and relay connectivity. No credentials are printed.');
  process.exitCode = 1;
}
