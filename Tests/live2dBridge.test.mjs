import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath, pathToFileURL } from 'node:url';
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
        const longAudio = snapshot({ timestamp: new Date().toISOString(), mouth_track: {
            started_at_ms: Date.now(), interval_ms: 50, values: Array(2400).fill(128)
        } });
        await writeFile(state, JSON.stringify(longAudio, null, 2));
        const longOutput = spawnSync(process.execPath, [main, '--state', state, '--inspect'], { encoding: 'utf8' });
        assert.equal(longOutput.status, 0, longOutput.stderr);
        assert.equal(values(JSON.parse(longOutput.stdout).inputs).ReviaMouthGate, 128 / 255);
        await writeFile(state, JSON.stringify(snapshot()) + ' '.repeat(65536));
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

test('mouth follows output-audio windows independently of concurrent generation status', () =>
{
    const audio = snapshot({ phase: 'responding', mouth_track: {
        started_at_ms: now, interval_ms: 50, values: [0, 128, 255, 0]
    } });
    assert.equal(values(mapSnapshot(audio, now)).ReviaMouthGate, 0);
    assert.equal(values(mapSnapshot(audio, now + 50)).ReviaMouthGate, 128 / 255);
    assert.equal(values(mapSnapshot(audio, now + 100)).ReviaMouthGate, 1);
    assert.equal(values(mapSnapshot(audio, now + 150)).ReviaMouthGate, 0);
    assert.equal(values(mapSnapshot(audio, now + 200)).ReviaMouthGate, 0);
    assert.equal(values(mapSnapshot({ ...audio, speaking: false }, now + 100)).ReviaMouthGate, 0);
});

test('a bounded output-audio track remains valid past the legacy speaking-gate deadline', () =>
{
    const audio = snapshot({ mouth_track: { started_at_ms: now, interval_ms: 50,
        values: Array(400).fill(128) } });
    assert.equal(values(mapSnapshot(audio, now + 16000)).ReviaMouthGate, 128 / 255);
    assert.equal(values(mapSnapshot(audio, now + 20000)).ReviaMouthGate, 0);
});

test('malformed or future output tracks close the mouth while retaining valid mood', () =>
{
    for (const track of [{}, { started_at_ms: now + 6000, interval_ms: 50, values: [255] },
        { started_at_ms: now, interval_ms: 0, values: [255] },
        { started_at_ms: now, interval_ms: 50, values: [NaN] },
        { started_at_ms: now, interval_ms: 50, values: [256] },
        { started_at_ms: now, interval_ms: 50, values: Array(2401).fill(255) }])
    {
        const output = values(mapSnapshot(snapshot({ mouth_track: track }), now));
        assert.equal(output.ReviaMouthGate, 0);
        assert.equal(output.ReviaJoy, 0.6);
    }
});

test('a disconnect between calls reports a transport failure instead of an authentication denial', async () =>
{
    const client = await connected();
    const auth = client.authenticate('private-token');
    client.socket.reply(client.socket.sent.at(-1), 'AuthenticationResponse', { authenticated: true });
    await auth;
    client.socket.close();
    await assert.rejects(client.inject(mapSnapshot(null)), /connection closed/);
});

const rendererLoader = `
export async function resolve(specifier, context, nextResolve)
{
    if (specifier === 'ws') return { url: 'revia-fixture:renderer', shortCircuit: true };
    return nextResolve(specifier, context);
}
export async function load(url, context, nextLoad)
{
    if (url === 'revia-fixture:renderer')
        return { format: 'module', source: 'export default globalThis.WebSocket;', shortCircuit: true };
    return nextLoad(url, context);
}
`;

const rendererPreload = `
import { EventEmitter } from 'node:events';
import { register } from 'node:module';
const scenario = process.env.REVIA_RENDERER_SCENARIO;
const report = { attempts: 0, maximumClients: 0, activeClients: 0, requests: [] };
process.once('exit', () => console.log('Renderer fixture: ' + JSON.stringify(report)));
globalThis.WebSocket = class extends EventEmitter
{
    readyState = 0;
    closed = false;
    constructor()
    {
        super();
        this.attempt = ++report.attempts;
        report.maximumClients = Math.max(report.maximumClients, ++report.activeClients);
        queueMicrotask(() =>
        {
            if (scenario === 'retry-unavailable' && this.attempt === 2)
            {
                this.readyState = 3;
                this.emit('error');
            }
            else
            {
                this.readyState = 1;
                this.emit('open');
            }
        });
    }
    addEventListener(type, callback) { this.on(type, callback); }
    removeEventListener(type, callback) { this.off(type, callback); }
    close()
    {
        if (this.closed) return;
        this.closed = true;
        this.readyState = 3;
        --report.activeClients;
        this.emit('close');
    }
    send(body)
    {
        const request = JSON.parse(body);
        report.requests.push({ attempt: this.attempt, type: request.messageType,
            faceFound: request.data.faceFound, values: request.data.parameterValues });
        let data = {};
        if (request.messageType === 'APIStateRequest')
            data = { active: true, vTubeStudioVersion: 'fixture', currentSessionAuthenticated: false };
        else if (request.messageType === 'AuthenticationRequest')
        {
            if (scenario === 'cancel-authentication' && this.attempt === 2)
            {
                setTimeout(() => process.emit('SIGTERM'), 10);
                return;
            }
            data = { authenticated: scenario !== 'deny-authentication' || this.attempt === 1,
                reason: 'Synthetic renderer authentication result.' };
        }
        else if (request.messageType === 'ParameterCreationRequest')
            data = { parameterName: request.data.parameterName };
        else if (request.messageType === 'CurrentModelRequest')
            data = { modelLoaded: true, modelName: 'Fixture', modelID: 'fixture',
                vtsModelName: 'fixture.vtube.json', vtsModelIconName: '', live2DModelName: 'fixture.model3.json',
                modelLoadTime: 0, modelPosition: { positionX: 0, positionY: 0, rotation: 0, size: 0 } };
        if (scenario === 'retry-unavailable' && this.attempt === 1 &&
            request.messageType === 'InjectParameterDataRequest' && request.data.faceFound && !this.scheduled)
        {
            this.scheduled = true;
            queueMicrotask(() => this.close());
            return;
        }
        queueMicrotask(() => this.emit('message', { data: JSON.stringify({ apiName: 'VTubeStudioPublicAPI',
            apiVersion: '1.0', requestID: request.requestID,
            messageType: request.messageType.replace(/Request$/, 'Response'), data }) }));
        if (request.messageType !== 'InjectParameterDataRequest' || !request.data.faceFound || this.scheduled) return;
        this.scheduled = true;
        if (scenario === 'cancel-active' || this.attempt > 1)
            setTimeout(() => process.emit('SIGTERM'), 10);
        else
        {
            setTimeout(() => this.close(), 10);
            if (scenario === 'cancel-retry') setTimeout(() => process.emit('SIGTERM'), 100);
        }
    }
};
register(new URL('./renderer-loader.mjs', import.meta.url));
`;

async function runRendererFixture(scenario)
{
    const fixture = await mkdtemp(join(tmpdir(), 'revia-live2d-runner-'));
    try
    {
        const state = join(fixture, 'avatar.json');
        const credentials = join(fixture, 'token.json');
        const preload = join(fixture, 'renderer.mjs');
        const loader = join(fixture, 'renderer-loader.mjs');
        await writeFile(state, JSON.stringify(snapshot({ timestamp: new Date().toISOString() })));
        await writeFile(credentials, JSON.stringify({ pluginName: 'Revia Presence', pluginDeveloper: 'Revia Project',
            authenticationToken: 'synthetic-runner-fixture' }));
        await writeFile(loader, rendererLoader);
        await writeFile(preload, rendererPreload);
        const main = fileURLToPath(new URL('../Tools/Presence/Live2D/main.mjs', import.meta.url));
        const output = spawnSync(process.execPath, ['--import', pathToFileURL(preload).href, main, '--state', state, '--auth-file', credentials],
            { encoding: 'utf8', timeout: 12000, env: { ...process.env, REVIA_RENDERER_SCENARIO: scenario } });
        assert.equal(output.error, undefined, output.error?.message);
        const summary = output.stdout.split('\n').find(line => line.startsWith('Renderer fixture: '));
        assert.ok(summary, output.stderr);
        return { ...output, renderer: JSON.parse(summary.slice('Renderer fixture: '.length)) };
    }
    finally { await rm(fixture, { recursive: true, force: true }); }
}

test('CLI reconnects after the renderer closes between polls and resumes owned input injection', async () =>
{
    const output = await runRendererFixture('close-between-polls');
    assert.equal(output.status, 0, output.stderr);
    assert.equal(output.renderer.attempts, 2);
    assert.equal(output.renderer.maximumClients, 1);
    assert.equal(output.renderer.activeClients, 0);
    const resumed = output.renderer.requests.filter(item => item.attempt === 2);
    assert.equal(resumed.filter(item => item.type === 'ParameterCreationRequest').length, 8);
    assert.ok(resumed.some(item => item.type === 'InjectParameterDataRequest' && item.faceFound));
    assert.ok(output.renderer.requests.every(item => item.type !== 'AuthenticationTokenRequest'));
});

test('CLI keeps retrying an established renderer while VTube Studio is still starting', async () =>
{
    const output = await runRendererFixture('retry-unavailable');
    assert.equal(output.status, 0, output.stderr);
    assert.equal(output.renderer.attempts, 3);
    assert.equal(output.renderer.maximumClients, 1);
    assert.equal(output.renderer.activeClients, 0);
    assert.ok(output.renderer.requests.some(item => item.attempt === 3 &&
        item.type === 'InjectParameterDataRequest' && item.faceFound));
});

test('CLI treats denied authentication on reconnect as terminal without requesting new approval', async () =>
{
    const output = await runRendererFixture('deny-authentication');
    assert.equal(output.status, 1);
    assert.match(output.stderr, /authentication denied or revoked/);
    assert.equal(output.renderer.attempts, 2);
    assert.equal(output.renderer.activeClients, 0);
    assert.ok(output.renderer.requests.every(item => item.type !== 'AuthenticationTokenRequest'));
    assert.ok(output.renderer.requests.filter(item => item.attempt === 2)
        .every(item => ['APIStateRequest', 'AuthenticationRequest'].includes(item.type)));
});

test('CLI cancellation during retry wait prevents another connection attempt', async () =>
{
    const output = await runRendererFixture('cancel-retry');
    assert.equal(output.status, 0, output.stderr);
    assert.equal(output.renderer.attempts, 1);
    assert.equal(output.renderer.activeClients, 0);
});

test('CLI cancellation during reconnect authentication prevents parameter initialization', async () =>
{
    const output = await runRendererFixture('cancel-authentication');
    assert.equal(output.status, 0, output.stderr);
    assert.equal(output.renderer.attempts, 2);
    assert.equal(output.renderer.activeClients, 0);
    assert.ok(output.renderer.requests.filter(item => item.attempt === 2)
        .every(item => ['APIStateRequest', 'AuthenticationRequest'].includes(item.type)));
});

test('CLI cancellation of a connected renderer sends neutral inputs before closing', async () =>
{
    const output = await runRendererFixture('cancel-active');
    assert.equal(output.status, 0, output.stderr);
    assert.equal(output.renderer.attempts, 1);
    assert.equal(output.renderer.activeClients, 0);
    const finalRequest = output.renderer.requests.at(-1);
    assert.equal(finalRequest.type, 'InjectParameterDataRequest');
    assert.equal(finalRequest.faceFound, false);
    assert.equal(finalRequest.values.length, 8);
    assert.ok(finalRequest.values.every(item => item.value === 0));
});

test('Windows launcher forwards explicit paths from another directory and propagates Node exit status', {
    skip: process.platform !== 'win32'
}, async () =>
{
    const fixture = await mkdtemp(join(tmpdir(), 'revia-live2d-launcher-'));
    try
    {
        const working = join(fixture, 'another working directory');
        await mkdir(working);
        const recorder = join(fixture, 'record-arguments.mjs');
        await writeFile(recorder, 'console.log(JSON.stringify(process.argv.slice(2))); process.exit(7);\n');
        await writeFile(join(fixture, 'node.cmd'), `@echo off\r\n"${process.execPath}" "${recorder}" %*\r\nexit /b %errorlevel%\r\n`);
        const script = fileURLToPath(new URL('../Tools/Presence/Live2D/StartLive2D.ps1', import.meta.url));
        const main = fileURLToPath(new URL('../Tools/Presence/Live2D/main.mjs', import.meta.url));
        const powershell = join(process.env.SystemRoot, 'System32', 'WindowsPowerShell', 'v1.0', 'powershell.exe');
        const state = join(working, 'avatar state.json');
        const credentials = join(working, 'unused credential destination.json');
        const run = (args, executablePath = `${fixture};${process.env.PATH}`) =>
            spawnSync(powershell, ['-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', script, ...args],
            { encoding: 'utf8', timeout: 10000, windowsHide: true, cwd: working,
                env: { ...process.env, PATH: executablePath } });
        const explicit = run(['-StatePath', state, '-Endpoint', 'ws://localhost:8002', '-AuthFile', credentials, '-Inspect']);
        assert.ifError(explicit.error);
        assert.equal(explicit.status, 7, explicit.stderr);
        assert.deepEqual(JSON.parse(explicit.stdout.trim()), [main, '--state', state,
            '--endpoint', 'ws://localhost:8002', '--auth-file', credentials, '--inspect']);
        const defaults = run(['-StatePath', state]);
        assert.equal(defaults.status, 7, defaults.stderr);
        assert.deepEqual(JSON.parse(defaults.stdout.trim()), [main, '--state', state]);
        const missingState = run([]);
        assert.equal(missingState.status, 1);
        assert.match(missingState.stderr, /StatePath/);
        const missingNode = run(['-StatePath', state], working);
        assert.equal(missingNode.status, 1);
        assert.match(missingNode.stderr, /Node\.js 22\.12 or newer must be available on PATH/);
        await assert.rejects(readFile(credentials), { code: 'ENOENT' });
    }
    finally { await rm(fixture, { recursive: true, force: true }); }
});
