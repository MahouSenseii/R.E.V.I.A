import assert from 'node:assert/strict';
import test from 'node:test';
import { VtsClient } from '../Tools/Presence/Live2D/client.mjs';

test('real VTube Studio returns three correlated API state replies on one connection', {
    skip: process.env.REVIA_LIVE_VTS !== '1', timeout: 10000
}, async context =>
{
    const client = new VtsClient('ws://127.0.0.1:8001', { timeoutMs: 2000 });
    try
    {
        await client.connect();
        for (let index = 0; index < 3; ++index)
        {
            const start = performance.now();
            const state = await client.request('APIStateRequest');
            assert.equal(state.active, true);
            context.diagnostic(`API state reply ${index + 1} received in ${Math.round(performance.now() - start)} ms.`);
        }
    }
    finally { client.close(); }
});
