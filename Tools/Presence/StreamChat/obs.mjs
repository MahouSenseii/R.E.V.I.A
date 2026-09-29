// OBS through obs-websocket 5 (built into OBS 28 and later), on Node's own WebSocket.
//
// Two jobs, both driven by files Revia already writes: when avatar_state.json reads
// "brb" (the operator's kill switch), switch to the BRB scene and back when it clears;
// and keep a text source showing caption.txt, the filtered reply she just gave.
import { createHash } from 'node:crypto';
import { readFile } from 'node:fs/promises';

// obs-websocket's challenge: base64(sha256(base64(sha256(password + salt)) + challenge)).
export function authenticationResponse(password, salt, challenge) {
  const secret = createHash('sha256').update(password + salt).digest('base64');
  return createHash('sha256').update(secret + challenge).digest('base64');
}

export function identifyMessage(hello, password) {
  const d = { rpcVersion: 1, eventSubscriptions: 0 };
  const auth = hello?.d?.authentication;
  if (auth) {
    if (!password) throw new Error('OBS: the server requires a password; set OBS_WEBSOCKET_PASSWORD.');
    d.authentication = authenticationResponse(password, auth.salt, auth.challenge);
  }
  return { op: 1, d };
}

export function requestMessage(requestType, requestData, requestId) {
  return { op: 6, d: { requestType, requestId, requestData } };
}

export class ObsClient {
  constructor(config, { password = process.env.OBS_WEBSOCKET_PASSWORD, WebSocketImpl = globalThis.WebSocket,
    log = console.log } = {}) {
    this.config = config; this.password = password; this.WebSocket = WebSocketImpl; this.log = log;
    this.socket = null; this.identified = false; this.nextId = 1; this.waiting = new Map();
    this.lastScene = null; this.lastCaption = null;
  }

  connect() {
    const url = this.config.url ?? 'ws://127.0.0.1:4455';
    return new Promise((resolve, reject) => {
      const socket = new this.WebSocket(url);
      this.socket = socket;
      socket.addEventListener('error', () => reject(new Error(`OBS: could not connect to ${url}.`)));
      socket.addEventListener('close', () => { this.identified = false; });
      socket.addEventListener('message', event => {
        const message = JSON.parse(typeof event.data === 'string' ? event.data : String(event.data));
        if (message.op === 0) socket.send(JSON.stringify(identifyMessage(message, this.password)));
        else if (message.op === 2) { this.identified = true; this.log('OBS: connected.'); resolve(); }
        else if (message.op === 7) {
          const pending = this.waiting.get(message.d.requestId);
          if (pending) { this.waiting.delete(message.d.requestId); pending(message.d); }
        }
      });
    });
  }

  request(requestType, requestData) {
    if (!this.identified) return Promise.resolve(null);
    const requestId = String(this.nextId++);
    return new Promise(resolve => {
      this.waiting.set(requestId, resolve);
      this.socket.send(JSON.stringify(requestMessage(requestType, requestData, requestId)));
      setTimeout(() => { if (this.waiting.delete(requestId)) resolve(null); }, 5000);
    });
  }

  async setScene(sceneName) {
    if (!sceneName || sceneName === this.lastScene) return;
    const response = await this.request('SetCurrentProgramScene', { sceneName });
    if (response?.requestStatus?.result) { this.lastScene = sceneName; this.log(`OBS: scene ${sceneName}.`); }
    else if (response) this.log(`OBS: could not switch to ${sceneName}: ${response.requestStatus?.comment ?? 'refused'}.`);
  }

  async setCaption(text) {
    if (!this.config.captionSource || text === this.lastCaption) return;
    const response = await this.request('SetInputSettings', {
      inputName: this.config.captionSource, inputSettings: { text }, overlay: true });
    if (response?.requestStatus?.result) this.lastCaption = text;
  }

  // One pass over the two files. Called on a timer by main; a test calls it directly.
  async sync({ statePath, captionPath }) {
    if (statePath && this.config.brbScene) {
      try {
        const state = JSON.parse(await readFile(statePath, 'utf8'));
        if (state.phase === 'brb') await this.setScene(this.config.brbScene);
        else if (this.lastScene === this.config.brbScene && this.config.liveScene) await this.setScene(this.config.liveScene);
      } catch (error) { if (error.code !== 'ENOENT' && !(error instanceof SyntaxError)) throw error; }
    }
    if (captionPath && this.config.captionSource) {
      try { await this.setCaption((await readFile(captionPath, 'utf8')).trim()); }
      catch (error) { if (error.code !== 'ENOENT') throw error; }
    }
  }

  close() { this.socket?.close(); }
}
