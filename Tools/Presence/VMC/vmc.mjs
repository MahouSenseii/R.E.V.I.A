// A VRM face through the VMC protocol (protocol.vmc.info): OSC over UDP to whatever
// renders the model -- VSeeFace, VNyan, Warudo, or anything else that listens as a
// Marionette on port 39539.
//
// Revia's core publishes what she is doing to avatar_state.json; this adapter turns it
// into blend shapes and a head bone: mouth from her speaking state, an expression from
// her emotion (weights per VRM blend shape, mapped by name), gaze from her attention,
// plus the blink and idle look-around that make a face read as alive. It never calls
// inference and never grants an action; it is a consumer of the avatar bridge and
// nothing else, exactly like the VTube Studio adapter beside it.

// OSC 1.0 encoding: strings NUL-padded to four bytes, big-endian int32 and float32.
export function oscString(text) {
  const bytes = Buffer.from(String(text), 'utf8');
  const padding = 4 - (bytes.length % 4);
  return Buffer.concat([bytes, Buffer.alloc(padding)]);
}

export function oscMessage(address, args = []) {
  const types = ',' + args.map(argument => argument.type).join('');
  const parts = [oscString(address), oscString(types)];
  for (const argument of args) {
    if (argument.type === 'i') { const b = Buffer.alloc(4); b.writeInt32BE(Math.trunc(argument.value)); parts.push(b); }
    else if (argument.type === 'f') { const b = Buffer.alloc(4); b.writeFloatBE(argument.value); parts.push(b); }
    else if (argument.type === 's') parts.push(oscString(argument.value));
    else throw new Error(`OSC: unsupported type ${argument.type}`);
  }
  return Buffer.concat(parts);
}

// A bundle with the "immediately" time tag; each element is length-prefixed.
export function oscBundle(messages) {
  const parts = [oscString('#bundle'), Buffer.from([0, 0, 0, 0, 0, 0, 0, 1])];
  for (const message of messages) {
    const length = Buffer.alloc(4);
    length.writeInt32BE(message.length);
    parts.push(length, message);
  }
  return Buffer.concat(parts);
}

const i = value => ({ type: 'i', value });
const f = value => ({ type: 'f', value });
const s = value => ({ type: 's', value });

// The two generations of VRM name the same shapes differently.
export const BLEND_SHAPE_SETS = {
  vrm0: { mouth: 'A', blinkLeft: 'Blink_L', blinkRight: 'Blink_R', joy: 'Joy', angry: 'Angry', sorrow: 'Sorrow', fun: 'Fun',
    surprised: null },
  vrm1: { mouth: 'aa', blinkLeft: 'blinkLeft', blinkRight: 'blinkRight', joy: 'happy', angry: 'angry', sorrow: 'sad',
    fun: 'relaxed', surprised: 'surprised' },
};

// Her expressions as the bridge names them (Config/avatar.json), as blend-shape
// weights. A config may override any entry by expression name.
export const DEFAULT_EXPRESSIONS = {
  neutral: {},
  focused: {},
  happy: { joy: 1 },
  excited: { joy: 0.8, surprised: 0.5 },
  playful: { fun: 0.8, joy: 0.3 },
  curious: { fun: 0.3, surprised: 0.3 },
  bored: { sorrow: 0.2 },
  sulky: { angry: 0.35, sorrow: 0.3 },
  sad: { sorrow: 0.9 },
  melancholy: { sorrow: 0.6 },
  lonely: { sorrow: 0.5 },
  angry: { angry: 0.9 },
  frustrated: { angry: 0.6 },
  concerned: { sorrow: 0.5 },
  confused: { sorrow: 0.2, fun: 0.1 },
};

// The mouth while she speaks: the same stand-in envelope the VTube Studio adapter
// uses until visemes exist -- syllable-rate movement with pauses.
export function mouthValue(speaking, elapsedMs, random = Math.random) {
  if (!speaking) return 0;
  const syllable = Math.sin(elapsedMs / 1000 * Math.PI * 2 * 4.5);
  const pause = Math.sin(elapsedMs / 1000 * Math.PI * 2 * 0.7) < -0.6 ? 0.15 : 1;
  return Math.max(0, Math.min(1, (0.45 + 0.45 * syllable) * pause + (random() - 0.5) * 0.1));
}

// A head turned yaw degrees (positive to her left) and pitch degrees (positive up), as
// the quaternion Unity expects: yaw about Y, then pitch about X.
export function headQuaternion(yawDegrees, pitchDegrees) {
  const yaw = yawDegrees * Math.PI / 180;
  const pitch = pitchDegrees * Math.PI / 180;
  const y = { x: 0, y: Math.sin(yaw / 2), z: 0, w: Math.cos(yaw / 2) };
  const p = { x: Math.sin(-pitch / 2), y: 0, z: 0, w: Math.cos(-pitch / 2) };
  return {
    x: y.w * p.x + y.x * p.w + y.y * p.z - y.z * p.y,
    y: y.w * p.y - y.x * p.z + y.y * p.w + y.z * p.x,
    z: y.w * p.z + y.x * p.y - y.y * p.x + y.z * p.w,
    w: y.w * p.w - y.x * p.x - y.y * p.y - y.z * p.z,
  };
}

export class VmcSender {
  constructor(config, { send, log = console.log, random = Math.random, now = () => Date.now() } = {}) {
    this.config = config; this.send = send; this.log = log; this.random = random; this.now = now;
    this.set = BLEND_SHAPE_SETS[config.blendShapeSet ?? 'vrm0'];
    if (!this.set) throw new Error(`VMC: blendShapeSet must be vrm0 or vrm1, not ${config.blendShapeSet}.`);
    this.expressions = { ...DEFAULT_EXPRESSIONS, ...(config.expressionBlendShapes ?? {}) };
    this.started = this.now();
    this.speakingSince = null; this.lastExpression = null; this.lastPhase = null;
    this.targets = new Map(); this.weights = new Map();
    this.nextBlinkAt = 0; this.blinkUntil = 0;
    this.gaze = { yaw: 0, pitch: 0 }; this.gazeTarget = { yaw: 0, pitch: 0 }; this.nextGazeAt = 0;
    this.sent = 0;
  }

  // One avatar state as the bridge writes it.
  applyState(state) {
    if (!state) return;
    const phase = state.phase ?? 'idle';
    const speaking = phase === 'speaking' || state.speaking === true;
    if (speaking && this.speakingSince === null) this.speakingSince = this.now();
    if (!speaking) this.speakingSince = null;
    this.lastPhase = phase;
    const expression = state.expression ?? 'neutral';
    const intensity = Math.max(0.35, Math.min(1, Number(state.affect_intensity ?? 1) || 1));
    const mapped = this.expressions[expression];
    if (mapped === undefined) {
      if (expression !== this.lastExpression) this.log(`VMC: no blend shapes mapped for expression ${expression}; left as is.`);
    } else {
      this.targets.clear();
      for (const [role, weight] of Object.entries(mapped)) {
        const name = this.set[role] ?? (role in this.set ? null : role);
        if (name) this.targets.set(name, Math.max(0, Math.min(1, weight * intensity)));
      }
      if (expression !== this.lastExpression) this.log(`VMC: expression ${expression} (${[...this.targets.keys()].join(', ') || 'neutral'}).`);
    }
    this.lastExpression = expression;
    // Where she looks: the local user by default, the stream when chat has her, up
    // and away while she thinks.
    const attention = String(state.attention ?? state.gaze_target ?? '');
    const target = attention.startsWith('stream') ? { yaw: 14, pitch: -3 }
      : phase === 'thinking' ? { yaw: -9, pitch: 10 }
      : { yaw: 0, pitch: 0 };
    if (target.yaw !== this.gazeTarget.yaw || target.pitch !== this.gazeTarget.pitch) {
      this.gazeTarget = target;
      this.nextGazeAt = 0;
    }
  }

  // Called at 30 Hz or so. Returns the messages sent, for the tests.
  tick() {
    const now = this.now();
    const speaking = this.speakingSince !== null;
    const mouth = mouthValue(speaking, speaking ? now - this.speakingSince : 0, this.random);
    if (now >= this.nextBlinkAt) { this.blinkUntil = now + 120; this.nextBlinkAt = now + 2500 + this.random() * 3500; }
    const blink = now < this.blinkUntil ? 1 : 0;
    if (now >= this.nextGazeAt) {
      this.gaze = { yaw: this.gazeTarget.yaw + (this.random() - 0.5) * 6, pitch: this.gazeTarget.pitch + (this.random() - 0.5) * 4 };
      this.nextGazeAt = now + 1200 + this.random() * 2800;
    }
    // Expressions ease toward their targets, and a shape no longer wanted eases to
    // zero before it is dropped, so a change of mood is a change of face, not a cut.
    for (const [name, target] of this.targets) {
      const current = this.weights.get(name) ?? 0;
      this.weights.set(name, current + (target - current) * 0.2);
    }
    for (const [name, current] of this.weights) {
      if (this.targets.has(name)) continue;
      const eased = current * 0.8;
      if (eased < 0.01) this.weights.delete(name); else this.weights.set(name, eased);
    }
    const head = headQuaternion(this.gaze.yaw, this.gaze.pitch);
    const messages = [
      { address: '/VMC/Ext/OK', args: [i(1)] },
      { address: '/VMC/Ext/T', args: [f((now - this.started) / 1000)] },
      { address: '/VMC/Ext/Bone/Pos', args: [s('Head'), f(0), f(0), f(0), f(head.x), f(head.y), f(head.z), f(head.w)] },
      { address: '/VMC/Ext/Blend/Val', args: [s(this.set.mouth), f(Number(mouth.toFixed(3)))] },
      { address: '/VMC/Ext/Blend/Val', args: [s(this.set.blinkLeft), f(blink)] },
      { address: '/VMC/Ext/Blend/Val', args: [s(this.set.blinkRight), f(blink)] },
    ];
    for (const [name, weight] of this.weights) {
      messages.push({ address: '/VMC/Ext/Blend/Val', args: [s(name), f(Number(weight.toFixed(3)))] });
    }
    messages.push({ address: '/VMC/Ext/Blend/Apply', args: [] });
    const encoded = messages.map(message => oscMessage(message.address, message.args));
    if (this.config.bundle === false) for (const datagram of encoded) this.send(datagram);
    else this.send(oscBundle(encoded));
    this.sent += 1;
    return messages;
  }
}
