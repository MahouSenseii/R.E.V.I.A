import { readFile, open, mkdir, rename, writeFile } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { setTimeout as delay } from 'node:timers/promises';
import { VtsClient, pluginIdentity, RendererTransportError } from './client.mjs';
import { mapSnapshot, validateEndpoint } from './state.mjs';

const root = fileURLToPath(new URL('../../../', import.meta.url));
const options = { state: resolve(root, 'RuntimeData/Presence/avatar_state.json'),
    authFile: resolve(root, 'RuntimeData/Presence/Live2D/vts_auth.json'), endpoint: 'ws://127.0.0.1:8001', inspect: false };

async function readSnapshot(path)
{
    try
    {
        const file = await open(path, 'r');
        try
        {
            const buffer = Buffer.alloc(65537);
            const { bytesRead } = await file.read(buffer, 0, buffer.length, 0);
            if (bytesRead > 65536) return null;
            return JSON.parse(buffer.subarray(0, bytesRead).toString('utf8'));
        }
        finally { await file.close(); }
    }
    catch { return null; }
}

function parseArguments(args)
{
    for (let index = 0; index < args.length; ++index)
    {
        const arg = args[index];
        if (arg === '--inspect') options.inspect = true;
        else if (['--state', '--auth-file', '--endpoint'].includes(arg))
        {
            const value = args[++index];
            if (!value || value.startsWith('--')) throw new Error(`Missing value for ${arg}.`);
            if (arg === '--state') options.state = resolve(value);
            if (arg === '--auth-file') options.authFile = resolve(value);
            if (arg === '--endpoint') options.endpoint = validateEndpoint(value);
        }
        else if (arg === '--help') return false;
        else throw new Error(`Unknown option ${arg}.`);
    }
    return true;
}

async function loadToken()
{
    try
    {
        const saved = JSON.parse(await readFile(options.authFile, 'utf8'));
        if (saved.pluginName !== pluginIdentity.pluginName || saved.pluginDeveloper !== pluginIdentity.pluginDeveloper ||
            typeof saved.authenticationToken !== 'string' || !saved.authenticationToken || saved.authenticationToken.length > 64)
            throw new Error();
        return saved.authenticationToken;
    }
    catch (error)
    {
        if (error.code === 'ENOENT') return null;
        throw new Error('The local VTube Studio credential file is invalid. Remove it explicitly to request approval again.');
    }
}

async function saveToken(token)
{
    await mkdir(dirname(options.authFile), { recursive: true });
    const temporary = `${options.authFile}.${process.pid}.tmp`;
    await writeFile(temporary, JSON.stringify({ ...pluginIdentity, authenticationToken: token }), { mode: 0o600 });
    await rename(temporary, options.authFile);
}

async function run()
{
    if (!parseArguments(process.argv.slice(2)))
    {
        console.log('Revia Live2D: node main.mjs [--state PATH] [--endpoint ws://127.0.0.1:8001] [--auth-file PATH] [--inspect]');
        return;
    }
    if (options.inspect)
    {
        console.log(JSON.stringify({ statePath: options.state, inputs: mapSnapshot(await readSnapshot(options.state)),
            note: 'Output-audio tracks use measured loudness and estimated playback timing; older voices use a gate.' }, null, 2));
        return;
    }
    const stop = new AbortController();
    let client;
    const onStop = () => { stop.abort(); client?.cancel(); };
    process.once('SIGINT', onStop);
    process.once('SIGTERM', onStop);
    try
    {
        let token = await loadToken();
        let established = false;
        while (!stop.signal.aborted)
        {
            client = new VtsClient(options.endpoint);
            try
            {
                await client.connect();
                const state = await client.request('APIStateRequest');
                if (state.active !== true) throw new Error('VTube Studio Plugin API is disabled.');
                if (!token)
                {
                    console.log('Approve Revia Presence in the VTube Studio plugin dialog.');
                    const response = await client.request('AuthenticationTokenRequest', pluginIdentity, 90000);
                    token = response.authenticationToken;
                    if (typeof token !== 'string' || !token || token.length > 64) throw new Error('No valid plugin token was returned.');
                    await client.authenticate(token);
                    stop.signal.throwIfAborted();
                    await saveToken(token);
                }
                else await client.authenticate(token);
                await client.initializeParameters();
                const model = await client.request('CurrentModelRequest');
                established = true;
                console.log(model.modelLoaded === true
                    ? 'Renderer connected. Verify that the loaded model maps the eight Revia inputs; model selection is manual.'
                    : 'Renderer connected without a model. Import the rigged Revia model and map the eight inputs.');
                while (!stop.signal.aborted)
                {
                    await client.inject(mapSnapshot(await readSnapshot(options.state)));
                    await delay(50, undefined, { signal: stop.signal });
                }
            }
            catch (error)
            {
                if (stop.signal.aborted) break;
                // Retry a lost session through renderer startup; approval failures stay terminal.
                if (!established || !(error instanceof RendererTransportError)) throw error;
                client.close();
                console.log('Renderer disconnected. Reconnecting with the existing approval in two seconds.');
                await delay(2000, undefined, { signal: stop.signal });
            }
        }
    }
    catch (error)
    {
        if (!stop.signal.aborted) throw error;
    }
    finally
    {
        process.removeListener('SIGINT', onStop);
        process.removeListener('SIGTERM', onStop);
        if (client) await client.shutdown().catch(() => client.close());
    }
}

run().catch(error =>
{
    console.error(`Revia Live2D: ${error.message}`);
    process.exitCode = 1;
});
