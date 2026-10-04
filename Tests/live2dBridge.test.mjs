import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import test from 'node:test';
import { mapSnapshot, parameterDefinitions, validateEndpoint } from '../Tools/Presence/Live2D/state.mjs';
import { VtsClient } from '../Tools/Presence/Live2D/client.mjs';

const now = Date.parse('2026-10-04T12:00:00Z');
const snapshot = overrides => ({ version: 1, sequence: 1, timestamp: new Date(now).toISOString(),
    phase: 'speaking', expression: 'happy', affect_intensity: 0.6, conversation_momentum: 0.4,
    speaking: true, listening: false, mouth: 1, gaze_target: 'user', ...overrides });
const values = result => Object.fromEntries(result.parameterValues.map(item => [item.id, item.value]));

test('uses owned tracking inputs, clamps bounds and keeps the mouth tied to the speaking phase', () =>
{
    const result = mapSnapshot(snapshot({ mouth: 4, affect_intensity: 4 }), now);
    assert.equal(result.faceFound, true);
    assert.equal(result.mode, 'set');
    assert.equal(values(result).ReviaMouthGate, 1);
    assert.equal(values(result).ReviaJoy, 1);
    assert.equal(values(mapSnapshot(snapshot({ phase: 'thinking' }), now)).ReviaMouthGate, 0);
    assert.deepEqual(Object.keys(values(result)).sort(), parameterDefinitions.map(item => item.parameterName).sort());
    assert.ok(Object.keys(values(result)).every(name => !name.startsWith('Param')));
});

test('reset on missing, malformed, unsupported, future and offline snapshots', () =>
{
    for (const input of [null, {}, snapshot({ version: 2 }), snapshot({ sequence: -1 }),
        snapshot({ timestamp: 'bad' }), snapshot({ timestamp: new Date(now + 60000).toISOString() }),
        snapshot({ phase: 'offline' }), snapshot({ phase: 'unrecognized' }), snapshot({ mouth: NaN })])
    {
        const result = mapSnapshot(input, now);
        assert.equal(result.faceFound, false);
        assert.ok(Object.values(values(result)).every(value => value === 0));
    }
});

test('transient mouth and attention expire; old event-only mood is retained', () =>
{
    const result = mapSnapshot(snapshot(), now + 16000);
    assert.equal(result.faceFound, true);
    assert.equal(values(result).ReviaMouthGate, 0);
    assert.equal(values(result).ReviaListening, 0);
    assert.equal(values(result).ReviaJoy, 0.6);
    const oldIdle = mapSnapshot(snapshot({ phase: 'idle', speaking: false }), now + 3600000);
    assert.equal(values(oldIdle).ReviaJoy, 0.6);
    assert.equal(oldIdle.faceFound, true);
    assert.equal(mapSnapshot(snapshot({ phase: 'thinking' }), now + 3600000).faceFound, true);
});

test('expression aliases and screen gaze use bounded independent signals', () =>
{
    assert.equal(values(mapSnapshot(snapshot({ expression: 'lonely' }), now)).ReviaSadness, 0.6);
    assert.equal(values(mapSnapshot(snapshot({ expression: 'focused', gaze_target: 'screen' }), now)).ReviaFocus, 0.6);
    assert.equal(values(mapSnapshot(snapshot({ gaze_target: 'screen' }), now)).ReviaGazeX, 0.35);
    assert.equal(values(mapSnapshot(snapshot({ expression: 'unknown' }), now)).ReviaJoy, 0);
});

test('renderer connections accept only plain loopback WebSockets', () =>
{
    for (const endpoint of ['ws://127.0.0.1:8001', 'ws://localhost:8001', 'ws://[::1]:8001'])
        assert.equal(validateEndpoint(endpoint), endpoint);
    for (const endpoint of ['ws://example.com:8001', 'wss://localhost:8001', 'ws://localhost.evil:8001',
        'ws://user:secret@localhost:8001', 'ws://localhost:8001/path', 'ws://localhost:8001?token=secret'])
        assert.throws(() => validateEndpoint(endpoint), /loopback/);
});

class FakeSocket extends EventEmitter
{
    readyState = 0;
    sent = [];
    constructor()
    {
        super();
        queueMicrotask(() => { this.readyState = 1; this.emit('open'); });
    }
    addEventListener(type, callback) { this.on(type, callback); }
    removeEventListener(type, callback) { this.off(type, callback); }
    send(body) { this.sent.push(JSON.parse(body)); }
    reply(request, messageType, data)
    {
        this.emit('message', { data: JSON.stringify({ apiName: 'VTubeStudioPublicAPI', apiVersion: '1.0',
            requestID: request.requestID, messageType, data }) });
    }
    close() { this.readyState = 3; this.emit('close'); }
}

async function connected(timeoutMs = 200)
{
    const client = new VtsClient('ws://127.0.0.1:8001', { socketFactory: () => new FakeSocket(), timeoutMs });
    await client.connect();
    return client;
}

test('correlates out-of-order replies and refuses unsolicited or wrong response types', async () =>
{
    const client = await connected();
    const first = client.request('APIStateRequest');
    const second = client.request('CurrentModelRequest');
    const [one, two] = client.socket.sent;
    client.socket.reply({ requestID: 'unrelated' }, 'APIStateResponse', {});
    client.socket.reply(two, 'CurrentModelResponse', { modelLoaded: false });
    client.socket.reply(one, 'APIStateResponse', { active: true });
    assert.equal((await first).active, true);
    assert.equal((await second).modelLoaded, false);
    const mismatch = client.request('APIStateRequest');
    client.socket.reply(client.socket.sent.at(-1), 'StatisticsResponse', {});
    await assert.rejects(mismatch, /Unexpected/);
    client.close();
});

test('rejects timeouts and pending calls on disconnect without leaking tokens', async () =>
{
    const client = await connected(15);
    await assert.rejects(client.request('APIStateRequest'), /timed out/);
    const pending = client.request('AuthenticationRequest', { authenticationToken: 'private-token' });
    client.close();
    await assert.rejects(pending, /closed/);
    const disconnected = await connected();
    const denied = disconnected.request('AuthenticationRequest');
    disconnected.socket.reply(disconnected.socket.sent.at(-1), 'APIError', { errorID: 50, message: 'private-token' });
    await assert.rejects(denied, error => error.message === 'VTube Studio API error 50');
    disconnected.close();
});

test('authentication is required before owned parameters are created or injected', async () =>
{
    const client = await connected();
    await assert.rejects(client.initializeParameters(), /authenticated/);
    await assert.rejects(client.inject(mapSnapshot(snapshot(), now)), /authenticated/);
    const auth = client.authenticate('private-token');
    client.socket.reply(client.socket.sent.at(-1), 'AuthenticationResponse', { authenticated: false });
    await assert.rejects(auth, /denied or revoked/);
    assert.equal(client.authenticated, false);
    client.close();
});

test('approved authentication initializes parameters and shutdown sends neutral values', async () =>
{
    const client = await connected();
    const auth = client.authenticate('private-token');
    client.socket.reply(client.socket.sent.at(-1), 'AuthenticationResponse', { authenticated: true });
    await auth;
    const initialization = client.initializeParameters();
    for (let index = 0; index < parameterDefinitions.length; ++index)
    {
        await new Promise(resolve => setImmediate(resolve));
        const request = client.socket.sent.at(-1);
        assert.equal(request.messageType, 'ParameterCreationRequest');
        assert.equal(request.data.parameterName, parameterDefinitions[index].parameterName);
        client.socket.reply(request, 'ParameterCreationResponse', {});
    }
    await initialization;
    const shutdown = client.shutdown();
    const request = client.socket.sent.at(-1);
    assert.equal(request.messageType, 'InjectParameterDataRequest');
    assert.equal(request.data.faceFound, false);
    assert.ok(request.data.parameterValues.every(item => item.value === 0));
    client.socket.reply(request, 'InjectParameterDataResponse', {});
    await shutdown;
    assert.equal(client.socket.readyState, 3);
});

test('real CLI inspect maps the selected source without touching credentials', async () =>
{
    const fixture = await mkdtemp(join(tmpdir(), 'revia-live2d-'));
    try
    {
        const state = join(fixture, 'avatar.json');
        const credentials = join(fixture, 'token.json');
        await writeFile(state, JSON.stringify(snapshot({ timestamp: new Date().toISOString() })));
        const main = fileURLToPath(new URL('../Tools/Presence/Live2D/main.mjs', import.meta.url));
        const output = spawnSync(process.execPath, [main, '--state', state, '--auth-file', credentials, '--inspect'], { encoding: 'utf8' });
        assert.equal(output.status, 0, output.stderr);
        assert.equal(values(JSON.parse(output.stdout).inputs).ReviaJoy, 0.6);
        await assert.rejects(readFile(credentials), { code: 'ENOENT' });
        await writeFile(state, JSON.stringify(snapshot()) + ' '.repeat(16384));
        const oversized = spawnSync(process.execPath, [main, '--state', state, '--inspect'], { encoding: 'utf8' });
        assert.equal(oversized.status, 0, oversized.stderr);
        assert.ok(Object.values(values(JSON.parse(oversized.stdout).inputs)).every(value => value === 0));
    }
    finally { await rm(fixture, { recursive: true, force: true }); }
});

test('CLI refuses bad arguments and remote endpoints before connecting', () =>
{
    const main = fileURLToPath(new URL('../Tools/Presence/Live2D/main.mjs', import.meta.url));
    for (const args of [['--state'], ['--unexpected'], ['--endpoint', 'ws://example.com:8001']])
    {
        const output = spawnSync(process.execPath, [main, ...args], { encoding: 'utf8' });
        assert.equal(output.status, 1);
        assert.match(output.stderr, /Missing value|Unknown option|loopback/);
    }
});

test('cancel during plugin approval rejects the dialog request and prohibits later startup mutations', async () =>
{
    const client = await connected();
    const approval = client.request('AuthenticationTokenRequest');
    const approvalRequest = client.socket.sent.at(-1);
    client.cancel();
    await assert.rejects(approval, /cancelled/);
    client.socket.reply(approvalRequest, 'AuthenticationTokenResponse', { authenticationToken: 'private-token' });
    await assert.rejects(client.authenticate('private-token'), /cancelled/);
    assert.equal(client.socket.sent.length, 1);
    assert.equal(client.authenticated, false);
    client.close();
});

test('cancel of an authenticated session still permits a bounded neutral shutdown', async () =>
{
    const client = await connected();
    const auth = client.authenticate('private-token');
    client.socket.reply(client.socket.sent.at(-1), 'AuthenticationResponse', { authenticated: true });
    await auth;
    client.cancel();
    await assert.rejects(client.inject(mapSnapshot(snapshot(), now)), /cancelled/);
    const shutdown = client.shutdown();
    const neutral = client.socket.sent.at(-1);
    assert.equal(neutral.messageType, 'InjectParameterDataRequest');
    assert.ok(neutral.data.parameterValues.every(item => item.value === 0));
    client.socket.reply(neutral, 'InjectParameterDataResponse', {});
    await shutdown;
    assert.equal(client.socket.readyState, 3);
});
