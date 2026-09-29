// Revia's face in VTube Studio.
//
//   node main.mjs config.json [--check]
//
// Watches the avatar state Revia writes and drives the Live2D model VTube Studio has
// loaded: mouth while she speaks, an expression hotkey for her emotion, gaze toward
// whoever has her attention, and the blink and idle look-around in between.
import { readFile, stat } from 'node:fs/promises';
import path from 'node:path';
import { VTubeStudioClient } from './vts.mjs';

const configPath = process.argv[2] ?? new URL('./config.json', import.meta.url);
const config = JSON.parse(await readFile(configPath, 'utf8'));
if (!path.isAbsolute(config.statePath ?? '')) throw new Error('Set statePath to the absolute path of Revia\'s avatar_state.json.');
if (config.tokenPath && !path.isAbsolute(config.tokenPath)) throw new Error('tokenPath must be absolute.');
if (!(await stat(config.statePath).catch(() => null))?.isFile())
  throw new Error(`Start Revia first: ${config.statePath} does not exist yet.`);
if (process.argv.includes('--check')) {
  console.log(`Configuration is valid; ${Object.keys(config.expressionHotkeys ?? {}).length} expressions mapped. No connection opened.`);
  process.exit(0);
}

const client = new VTubeStudioClient(config);
const shutdown = new AbortController();
let stopping = false;
function stop(reason) { if (stopping) return; stopping = true; shutdown.abort(); client.close(); console.log(reason); }
process.once('SIGINT', () => stop('VTube Studio adapter stopped.'));
process.once('SIGTERM', () => stop('VTube Studio adapter stopped.'));

await client.connect();
await client.authenticate();
let lastSequence = -1;
let lastRead = 0;
while (!shutdown.signal.aborted) {
  const started = Date.now();
  try {
    if (started - lastRead >= 100) {
      lastRead = started;
      const state = JSON.parse(await readFile(config.statePath, 'utf8'));
      if (state.sequence !== lastSequence) { lastSequence = state.sequence; await client.applyState(state); }
    }
    await client.tick();
  } catch (error) {
    if (!(error instanceof SyntaxError)) console.log(`VTube Studio: ${error.message}`);
  }
  const elapsed = Date.now() - started;
  await new Promise(resolve => setTimeout(resolve, Math.max(5, 33 - elapsed)));
}
