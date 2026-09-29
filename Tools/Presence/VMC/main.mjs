// Revia's face on a VRM model, through the VMC protocol.
//
//   node main.mjs config.json [--check]
//
// Watches the avatar state Revia writes and sends blend shapes and a head bone over
// OSC/UDP to a VMC receiver (VSeeFace, VNyan, Warudo, ...): mouth while she speaks, an
// expression for her emotion, gaze toward whoever has her attention, and the blink and
// idle look-around in between.
import { readFile, stat } from 'node:fs/promises';
import dgram from 'node:dgram';
import path from 'node:path';
import { VmcSender } from './vmc.mjs';

const configPath = process.argv[2] ?? new URL('./config.json', import.meta.url);
const config = JSON.parse(await readFile(configPath, 'utf8'));
if (!path.isAbsolute(config.statePath ?? '')) throw new Error('Set statePath to the absolute path of Revia\'s avatar_state.json.');
const host = config.host ?? '127.0.0.1';
const port = Number(config.port ?? 39539);
if (!Number.isInteger(port) || port < 1 || port > 65535) throw new Error('port must be a UDP port number (VMC receivers listen on 39539 by default).');
if (!(await stat(config.statePath).catch(() => null))?.isFile())
  throw new Error(`Start Revia first: ${config.statePath} does not exist yet.`);
if (process.argv.includes('--check')) {
  new VmcSender(config, { send: () => {}, log: () => {} });
  console.log(`Configuration is valid; sending to ${host}:${port} as ${config.blendShapeSet ?? 'vrm0'} blend shapes. Nothing sent.`);
  process.exit(0);
}

const socket = dgram.createSocket('udp4');
const sender = new VmcSender(config, { send: datagram => socket.send(datagram, port, host) });
const shutdown = new AbortController();
let stopping = false;
function stop(reason) { if (stopping) return; stopping = true; shutdown.abort(); socket.close(); console.log(reason); }
process.once('SIGINT', () => stop('VMC adapter stopped.'));
process.once('SIGTERM', () => stop('VMC adapter stopped.'));
console.log(`VMC: sending to ${host}:${port}; start your VMC receiver there.`);

let lastSequence = -1;
let lastRead = 0;
while (!shutdown.signal.aborted) {
  const started = Date.now();
  try {
    if (started - lastRead >= 100) {
      lastRead = started;
      const state = JSON.parse(await readFile(config.statePath, 'utf8'));
      if (state.sequence !== lastSequence) { lastSequence = state.sequence; sender.applyState(state); }
    }
    sender.tick();
  } catch (error) {
    if (!(error instanceof SyntaxError)) console.log(`VMC: ${error.message}`);
  }
  const elapsed = Date.now() - started;
  await new Promise(resolve => setTimeout(resolve, Math.max(5, 33 - elapsed)));
}
