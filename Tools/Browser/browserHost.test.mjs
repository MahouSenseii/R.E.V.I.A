import assert from 'node:assert/strict';
import test from 'node:test';

import {
  basicPublicUrlPolicy,
  isPublicIpAddress,
  unwrapDuckDuckGoUrl,
} from './browserHost.mjs';

test('public-address policy rejects local, private, reserved, and link-local targets', () => {
  for (const address of [
    '127.0.0.1', '10.0.0.1', '172.16.2.3', '192.168.1.3', '169.254.2.1',
    '100.64.0.1', '0.0.0.0', '::1', 'fc00::1', 'fe80::1', '2001:db8::1',
    '::ffff:127.0.0.1', '::ffff:7f00:1', '64:ff9b::a00:1',
  ]) assert.equal(isPublicIpAddress(address), false, address);
  assert.equal(isPublicIpAddress('8.8.8.8'), true);
  assert.equal(isPublicIpAddress('2606:4700:4700::1111'), true);
});

test('URL policy exposes only ordinary public web targets', () => {
  for (const target of [
    'file:///C:/secret.txt', 'javascript:alert(1)', 'data:text/plain,hello',
    'https://localhost/', 'https://router.local/', 'https://127.0.0.1/',
    'https://192.168.1.1/', 'https://user:pass@example.com/',
    'https://example.com:8443/',
  ]) assert.equal(basicPublicUrlPolicy(target), null, target);
  assert.equal(basicPublicUrlPolicy('http://example.com/', { httpsOnly: true }), null);
  assert.equal(basicPublicUrlPolicy('https://example.com/path')?.href, 'https://example.com/path');
});

test('DuckDuckGo redirect links unwrap without evaluating page-provided code', () => {
  assert.equal(
    unwrapDuckDuckGoUrl('https://duckduckgo.com/l/?uddg=https%3A%2F%2Fexample.com%2Fstory'),
    'https://example.com/story',
  );
});
