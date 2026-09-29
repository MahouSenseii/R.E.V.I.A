import test from 'node:test';
import assert from 'node:assert/strict';
import dgram from 'node:dgram';
import { oscString, oscMessage, oscBundle, mouthValue, headQuaternion, VmcSender, BLEND_SHAPE_SETS } from './vmc.mjs';

// An OSC reader written apart from the encoder, so the two are checked against each
// other and against the OSC 1.0 layout rather than only against themselves.
function readString(buffer, offset) {
  const end = buffer.indexOf(0, offset);
  const text = buffer.toString('utf8', offset, end);
  return { text, next: end + (4 - (end % 4)) };
}
function decodeMessage(buffer) {
  const address = readString(buffer, 0);
  const types = readString(buffer, address.next);
  const args = [];
  let offset = types.next;
  for (const type of types.text.slice(1)) {
    if (type === 'i') { args.push(buffer.readInt32BE(offset)); offset += 4; }
    else if (type === 'f') { args.push(buffer.readFloatBE(offset)); offset += 4; }
    else if (type === 's') { const value = readString(buffer, offset); args.push(value.text); offset = value.next; }
    else throw new Error(`unknown type ${type}`);
  }
  return { address: address.text, args };
}
function decode(buffer) {
  if (buffer.toString('utf8', 0, 7) !== '#bundle') return [decodeMessage(buffer)];
  const messages = [];
  let offset = 16;
  while (offset < buffer.length) {
    const length = buffer.readInt32BE(offset);
    messages.push(decodeMessage(buffer.subarray(offset + 4, offset + 4 + length)));
    offset += 4 + length;
  }
  return messages;
}

test('OSC strings are NUL-padded to four bytes and messages carry big-endian arguments', () => {
  assert.deepEqual([...oscString('abc')], [97, 98, 99, 0]);
  assert.deepEqual([...oscString('abcd')], [97, 98, 99, 100, 0, 0, 0, 0]);
  const message = oscMessage('/VMC/Ext/Blend/Val', [{ type: 's', value: 'Joy' }, { type: 'f', value: 0.5 }]);
  assert.equal(message.length, 20 + 4 + 4 + 4);
  assert.deepEqual(decodeMessage(message), { address: '/VMC/Ext/Blend/Val', args: ['Joy', 0.5] });
  const ok = oscMessage('/VMC/Ext/OK', [{ type: 'i', value: 1 }]);
  assert.deepEqual([...ok.subarray(-4)], [0, 0, 0, 1]);
  const bundle = oscBundle([message, ok]);
  assert.equal(bundle.toString('utf8', 0, 7), '#bundle');
  assert.deepEqual(decode(bundle).map(m => m.address), ['/VMC/Ext/Blend/Val', '/VMC/Ext/OK']);
});

test('the mouth is shut when silent and moves at syllable rate when speaking', () => {
  assert.equal(mouthValue(false, 500), 0);
  const values = [];
  for (let ms = 0; ms < 1000; ms += 33) values.push(mouthValue(true, ms, () => 0.5));
  assert.ok(Math.max(...values) > 0.7 && Math.min(...values) < 0.2, 'the envelope did not open and close');
});

test('a turned head is a unit quaternion that turns about Y for yaw and X for pitch', () => {
  const straight = headQuaternion(0, 0);
  assert.deepEqual(straight, { x: 0, y: 0, z: 0, w: 1 });
  const left = headQuaternion(30, 0);
  assert.ok(left.y > 0.25 && Math.abs(left.x) < 1e-9 && Math.abs(left.z) < 1e-9);
  const up = headQuaternion(0, 20);
  assert.ok(Math.abs(up.x) > 0.15 && Math.abs(up.y) < 1e-9);
  const both = headQuaternion(30, 20);
  const norm = Math.hypot(both.x, both.y, both.z, both.w);
  assert.ok(Math.abs(norm - 1) < 1e-9, 'not a unit quaternion');
});

test('states become blend shapes and a head bone, sent as OSC bundles over UDP', async () => {
  const receiver = dgram.createSocket('udp4');
  const datagrams = [];
  receiver.on('message', datagram => datagrams.push(datagram));
  await new Promise(resolve => receiver.bind(0, '127.0.0.1', resolve));
  const port = receiver.address().port;
  const socket = dgram.createSocket('udp4');
  try {
    let clock = 10000;
    const logged = [];
    const sender = new VmcSender({ blendShapeSet: 'vrm0', expressionBlendShapes: { focused: { fun: 0.2 } } },
      { send: datagram => socket.send(datagram, port, '127.0.0.1'), log: line => logged.push(line), random: () => 0.5, now: () => clock });

    sender.applyState({ sequence: 1, phase: 'idle', expression: 'happy', affect_intensity: 0.8, attention: 'local user' });
    const first = sender.tick();
    const shapes = Object.fromEntries(first.filter(m => m.address === '/VMC/Ext/Blend/Val').map(m => [m.args[0].value, m.args[1].value]));
    assert.equal(shapes.A, 0, 'the mouth moved while silent');
    assert.equal(shapes.Blink_L, 1, 'the first tick did not blink');
    assert.ok(shapes.Joy > 0.1 && shapes.Joy < 0.8, `Joy did not start easing in: ${shapes.Joy}`);
    assert.equal(first[0].address, '/VMC/Ext/OK');
    assert.equal(first.at(-1).address, '/VMC/Ext/Blend/Apply');
    assert.ok(logged.some(line => line.includes('expression happy (Joy)')));

    for (let step = 0; step < 30; step += 1) { clock += 33; sender.tick(); }
    const settled = Object.fromEntries(sender.tick().filter(m => m.address === '/VMC/Ext/Blend/Val').map(m => [m.args[0].value, m.args[1].value]));
    assert.ok(Math.abs(settled.Joy - 0.8) < 0.02, `Joy did not settle at intensity: ${settled.Joy}`);
    assert.equal(settled.Blink_L, 0, 'the blink did not end');

    sender.applyState({ sequence: 2, phase: 'speaking', expression: 'sad', attention: 'stream:live' });
    clock += 120;
    const talking = sender.tick();
    const talkingShapes = Object.fromEntries(talking.filter(m => m.address === '/VMC/Ext/Blend/Val').map(m => [m.args[0].value, m.args[1].value]));
    assert.ok(talkingShapes.A > 0.3, 'the mouth stayed shut while speaking');
    assert.ok(talkingShapes.Sorrow > 0.1 && talkingShapes.Joy < 0.8, 'the change of mood was a cut, not an easing');
    const head = talking.find(m => m.address === '/VMC/Ext/Bone/Pos');
    assert.equal(head.args[0].value, 'Head');
    assert.ok(head.args[5].value > 0.05, 'she did not turn toward the stream');
    for (let step = 0; step < 40; step += 1) { clock += 33; sender.tick(); }
    const later = Object.fromEntries(sender.tick().filter(m => m.address === '/VMC/Ext/Blend/Val').map(m => [m.args[0].value, m.args[1].value]));
    assert.equal(later.Joy, undefined, 'a shape no longer wanted was not released');

    sender.applyState({ sequence: 3, phase: 'idle', expression: 'never-heard-of' });
    assert.ok(logged.some(line => line.includes('no blend shapes mapped')));
    sender.applyState({ sequence: 4, phase: 'idle', expression: 'focused' });
    clock += 33;
    const focused = Object.fromEntries(sender.tick().filter(m => m.address === '/VMC/Ext/Blend/Val').map(m => [m.args[0].value, m.args[1].value]));
    assert.ok(focused.Fun > 0, 'the config override for focused was ignored');

    await new Promise(resolve => setTimeout(resolve, 150));
    assert.ok(datagrams.length >= 70, `only ${datagrams.length} datagrams arrived`);
    const decoded = decode(datagrams[0]);
    assert.deepEqual(decoded[0], { address: '/VMC/Ext/OK', args: [1] });
    assert.equal(decoded[2].address, '/VMC/Ext/Bone/Pos');
    assert.equal(decoded[2].args[0], 'Head');
    assert.ok(decoded.some(m => m.address === '/VMC/Ext/Blend/Val' && m.args[0] === 'Joy'));
    assert.equal(decoded.at(-1).address, '/VMC/Ext/Blend/Apply');
  } finally {
    socket.close();
    receiver.close();
  }
});

test('vrm1 names are used when asked, one datagram per message when bundles are off', () => {
  const sent = [];
  const sender = new VmcSender({ blendShapeSet: 'vrm1', bundle: false }, { send: d => sent.push(d), log: () => {}, random: () => 0.5, now: () => 5000 });
  sender.applyState({ sequence: 1, phase: 'speaking', expression: 'angry' });
  const messages = sender.tick();
  const names = messages.filter(m => m.address === '/VMC/Ext/Blend/Val').map(m => m.args[0].value);
  assert.deepEqual(names, ['aa', 'blinkLeft', 'blinkRight', 'angry']);
  assert.equal(sent.length, messages.length, 'bundles off did not send one datagram per message');
  assert.equal(BLEND_SHAPE_SETS.vrm0.surprised, null, 'VRM0 has no surprised shape and must not invent one');
  assert.throws(() => new VmcSender({ blendShapeSet: 'vrm9' }, { send: () => {} }), /vrm0 or vrm1/);
});
