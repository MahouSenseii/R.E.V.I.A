import test from 'node:test';
import assert from 'node:assert/strict';
import { validateOperation, allowedOrigin, publicAddress } from './interactiveHost.mjs';

const grant = { enabled: true, navigate: true, interact: true,
  approvedOrigins: ['https://example.com', 'http://127.0.0.1:8080'], allowLoopback: false,
  timeoutMs: 5000, maxTextBytes: 4096, maxElements: 40, maxValueBytes: 4096 };

test('interactive permissions do not inherit research or private network access', () => {
  assert.equal(allowedOrigin('https://example.com/page', grant), true);
  for (const url of ['http://example.com', 'https://example.com.evil.test', 'file:///x', 'javascript:alert(1)',
    'https://owner:secret@example.com', 'http://127.0.0.1:8080']) assert.equal(allowedOrigin(url, grant), false);
  assert.equal(allowedOrigin('http://127.0.0.1:8080/x', { ...grant, allowLoopback: true }), true);
  for (const ip of ['127.0.0.1', '10.0.0.1', '192.168.1.1', '169.254.1.1', '::1', 'fd00::1', '::ffff:127.0.0.1'])
    assert.equal(publicAddress(ip), false);
  assert.equal(publicAddress('8.8.8.8'), true);
});
test('typed effects require a current owned observation and bounded text', () => {
  const base = { operation: 'fill', url: 'https://example.com', session: 'owned', generation: 3, element: '3-1', value: 'text', grant };
  const current = { session: 'owned', generation: 3 };
  assert.equal(validateOperation(base, current), '');
  for (const patch of [{ session: 'other' }, { generation: 2 }, { element: '' }, { script: 'alert(1)' },
    { value: 'x'.repeat(4097) }, { grant: { ...grant, interact: false } }, { grant: { ...grant, enabled: false } }])
    assert.notEqual(validateOperation({ ...base, ...patch }, current), '');
});
