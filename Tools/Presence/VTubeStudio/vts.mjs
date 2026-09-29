// VTube Studio over its public API (WebSocket on ws://localhost:8001), on Node's own
// WebSocket.
//
// Revia's core publishes what she is doing to avatar_state.json; this adapter turns it
// into a Live2D model moving in VTube Studio: mouth from her speaking state, an
// expression hotkey from her emotion, gaze from her attention, plus the blink and idle
// look-around that make a face read as alive. It never calls inference and never
// grants an action; it is a consumer of the avatar bridge and nothing else.
//
// Two facts about the API shape everything here: injected parameters revert unless
// they are re-sent at least once a second, and there is no audio lip-sync request, so
// the mouth is driven from timing rather than from an audio device.
import { readFile, writeFile } from 'node:fs/promises';

const API = { apiName: 'VTubeStudioPublicAPI', apiVersion: '1.0' };

export function request(messageType, data, requestID) {
  return { ...API, requestID, messageType, data };
}

// The mouth while she speaks: a stand-in envelope until visemes exist, shaped so it
// reads as talking rather than flapping -- syllable-rate movement with pauses.
export function mouthValue(speaking, elapsedMs, random = Math.random) {
  if (!speaking) return 0;
  const syllable = Math.sin(elapsedMs / 1000 * Math.PI * 2 * 4.5);
  const pause = Math.sin(elapsedMs / 1000 * Math.PI * 2 * 0.7) < -0.6 ? 0.15 : 1;
  return Math.max(0, Math.min(1, (0.45 + 0.45 * syllable) * pause + (random() - 0.5) * 0.1));
}

export class VTubeStudioClient {
  constructor(config, { WebSocketImpl = globalThis.WebSocket, log = console.log, random = Math.random,
    now = () => Date.now() } = {}) {
    this.config = config; this.WebSocket = WebSocketImpl; this.log = log; this.random = random; this.now = now;
    this.socket = null; this.nextId = 1; this.waiting = new Map(); this.authenticated = false;
    this.hotkeys = new Map(); this.lastExpression = null; this.lastPhase = null;
    this.speakingSince = null; this.nextBlinkAt = 0; this.blinkUntil = 0;
    this.gaze = { x: 0, y: 0 }; this.gazeTarget = { x: 0, y: 0 }; this.nextGazeAt = 0;
  }

  connect() {
    const url = this.config.url ?? 'ws://127.0.0.1:8001';
    return new Promise((resolve, reject) => {
      const socket = new this.WebSocket(url);
      this.socket = socket;
      socket.addEventListener('error', () => reject(new Error(`VTube Studio: could not connect to ${url}. Is it running with the API enabled?`)));
      socket.addEventListener('close', () => { this.authenticated = false; });
      socket.addEventListener('message', event => {
        const message = JSON.parse(typeof event.data === 'string' ? event.data : String(event.data));
        const pending = this.waiting.get(message.requestID);
        if (pending) { this.waiting.delete(message.requestID); pending(message); }
      });
      socket.addEventListener('open', () => resolve());
    });
  }

  send(messageType, data = {}) {
    const requestID = `revia-${this.nextId++}`;
    return new Promise((resolve, reject) => {
      this.waiting.set(requestID, resolve);
      this.socket.send(JSON.stringify(request(messageType, data, requestID)));
      setTimeout(() => { if (this.waiting.delete(requestID)) reject(new Error(`VTube Studio: no answer to ${messageType}.`)); }, 15000);
    });
  }

  // The token flow: the first time, VTube Studio shows a popup and the user clicks
  // Allow; the token is kept in the file the config names and reused after that.
  async authenticate() {
    const plugin = { pluginName: this.config.pluginName ?? 'Revia', pluginDeveloper: this.config.pluginDeveloper ?? 'Revia' };
    let token = null;
    if (this.config.tokenPath) token = (await readFile(this.config.tokenPath, 'utf8').catch(() => '')).trim() || null;
    if (!token) {
      this.log('VTube Studio: asking for permission; click Allow in VTube Studio.');
      const issued = await this.send('AuthenticationTokenRequest', { ...plugin, pluginIcon: null });
      token = issued.data?.authenticationToken;
      if (!token) throw new Error(`VTube Studio: permission was not granted (${issued.data?.message ?? issued.messageType}).`);
      if (this.config.tokenPath) await writeFile(this.config.tokenPath, token, { mode: 0o600 });
    }
    const answer = await this.send('AuthenticationRequest', { ...plugin, authenticationToken: token });
    if (!answer.data?.authenticated) {
      // A stale token: drop it and ask once more.
      if (this.config.tokenPath) await writeFile(this.config.tokenPath, '', { mode: 0o600 });
      throw new Error(`VTube Studio: authentication failed (${answer.data?.reason ?? 'no reason given'}). Run again to be asked for permission.`);
    }
    this.authenticated = true;
    const hotkeys = await this.send('HotkeysInCurrentModelRequest', {});
    this.hotkeys.clear();
    for (const hotkey of hotkeys.data?.availableHotkeys ?? []) this.hotkeys.set(hotkey.name, hotkey.hotkeyID);
    this.log(`VTube Studio: connected to ${hotkeys.data?.modelName ?? 'the current model'} with ${this.hotkeys.size} hotkeys.`);
  }

  // One avatar state as the bridge writes it. Expressions go through hotkeys the
  // config maps by name; a missing mapping is skipped, never guessed.
  async applyState(state) {
    if (!this.authenticated || !state) return;
    const phase = state.phase ?? 'idle';
    const speaking = phase === 'speaking' || state.speaking === true;
    if (speaking && this.speakingSince === null) this.speakingSince = this.now();
    if (!speaking) this.speakingSince = null;
    this.lastPhase = phase;
    const expression = state.expression ?? 'neutral';
    if (expression !== this.lastExpression) {
      const name = this.config.expressionHotkeys?.[expression];
      const hotkeyID = name ? this.hotkeys.get(name) : undefined;
      if (hotkeyID) {
        await this.send('HotkeyTriggerRequest', { hotkeyID });
        this.log(`VTube Studio: expression ${expression} (${name}).`);
      }
      this.lastExpression = expression;
    }
    // Where she looks: the local user by default, the stream when chat has her.
    const attention = String(state.attention ?? state.gaze_target ?? '');
    const target = attention.startsWith('stream') ? { x: 12, y: -4 }
      : phase === 'thinking' ? { x: -8, y: 10 }
      : { x: 0, y: 0 };
    if (target.x !== this.gazeTarget.x || target.y !== this.gazeTarget.y) {
      // A change of attention moves the eyes now, not at the next idle wander.
      this.gazeTarget = target;
      this.nextGazeAt = 0;
    }
  }

  // Called at 30 Hz or so: mouth, blink and idle gaze, re-sent every tick because
  // injected values revert after a second of silence.
  async tick() {
    if (!this.authenticated) return null;
    const now = this.now();
    const speaking = this.speakingSince !== null;
    const mouth = mouthValue(speaking, speaking ? now - this.speakingSince : 0, this.random);
    if (now >= this.nextBlinkAt) { this.blinkUntil = now + 120; this.nextBlinkAt = now + 2500 + this.random() * 3500; }
    const eyes = now < this.blinkUntil ? 0 : 1;
    if (now >= this.nextGazeAt) {
      // A small wander around the target, so the eyes are never pinned.
      this.gaze = { x: this.gazeTarget.x + (this.random() - 0.5) * 6, y: this.gazeTarget.y + (this.random() - 0.5) * 4 };
      this.nextGazeAt = now + 1200 + this.random() * 2800;
    }
    const parameterValues = [
      { id: 'MouthOpen', value: Number(mouth.toFixed(3)), weight: 1 },
      { id: 'EyeOpenLeft', value: eyes, weight: 1 },
      { id: 'EyeOpenRight', value: eyes, weight: 1 },
      { id: 'FaceAngleX', value: Number(this.gaze.x.toFixed(2)), weight: 1 },
      { id: 'FaceAngleY', value: Number(this.gaze.y.toFixed(2)), weight: 1 },
    ];
    await this.send('InjectParameterDataRequest', { faceFound: false, mode: 'set', parameterValues });
    return parameterValues;
  }

  close() { this.socket?.close(); }
}
