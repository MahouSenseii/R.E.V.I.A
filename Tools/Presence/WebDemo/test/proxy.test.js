import test from 'node:test';
import assert from 'node:assert/strict';
import { createRelay } from '../relay/server.js';
import { sourceAddress, trustedProxies } from '../relay/proxy.js';

test('only an explicitly trusted peer can supply a bounded XFF chain for network rate limits', async t => {
  const config = { mode: 'development', bind: '127.0.0.1', origins: ['http://127.0.0.1:9000'], hostId: 'demo', hostToken: 'host-secret-'.repeat(4), inviteCode: 'invite-secret-'.repeat(4), limits: { statusPerMinute: 1 } };
  for (const trusted of [false, true]) {
    await t.test(trusted ? 'trusted peer' : 'untrusted peer', async t => {
      const relay = createRelay({ ...config, trustedProxyCidrs: trusted ? ['127.0.0.1/32', '10.0.0.0/24'] : [] });
      await relay.listen(0); t.after(() => relay.close());
      const get = xff => fetch(`http://127.0.0.1:${relay.server.address().port}/v1/status`, { headers: { 'X-Forwarded-For': xff } });
      assert.equal((await get('1.1.1.1, 198.51.100.10, 10.0.0.2')).status, 200);
      assert.equal((await get('2.2.2.2, 198.51.100.10, 10.0.0.2')).status, 429, 'Spoofed leftmost address must not rotate the budget');
      assert.equal((await get('198.51.100.11, 10.0.0.2')).status, trusted ? 200 : 429);
    });
  }
});

test('invalid proxy chains fall back safely and equivalent IPv6 sources share one identity', () => {
  const trusted = trustedProxies(['127.0.0.1/32', '2001:db8:ffff::/48']);
  const req = xff => ({ socket: { remoteAddress: '127.0.0.1' }, headers: { 'x-forwarded-for': xff } });
  for (const value of ['fe80::1%zone', 'unknown', '1.2.3.4,'.repeat(17) + '1.2.3.4', '1.2.3.999']) assert.equal(sourceAddress(req(value), trusted), '127.0.0.1');
  assert.equal(sourceAddress(req('2001:0db8:0000:0000:0000:0000:0000:0001, 2001:db8:ffff::1'), trusted), '2001:db8::1');
  for (const cidr of ['0.0.0.0/0', '::/0', '*', '10.0.0.0/33', '::/129']) assert.throws(() => trustedProxies([cidr]));
});
