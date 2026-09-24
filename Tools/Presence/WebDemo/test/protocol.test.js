import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { frame, messageBody } from '../protocol/index.js';
const fixtures = JSON.parse(await readFile(new URL('../protocol/fixtures.json', import.meta.url), 'utf8'));
test('all wire fixtures validate and protocol versions, extra authority and direction swaps fail closed', () => {
  for (const fixture of fixtures.valid) {
    assert.equal(frame(fixture.frame, fixture.direction), true);
    assert.equal(frame({ ...fixture.frame, role: 'owner' }, fixture.direction), false);
    assert.equal(frame({ ...fixture.frame, version: 2 }, fixture.direction), false);
    assert.equal(frame({ ...fixture.frame, epoch: '../path' }, fixture.direction), false);
    assert.equal(frame(fixture.frame, fixture.direction === 'host' ? 'relay' : 'host'), false);
  }
});
test('Unicode limits count code points, reject lone surrogates and forbid hidden authority fields', () => {
  assert(messageBody({ text: '😀'.repeat(2000), idempotencyKey: fixtures.requestId }));
  assert(!messageBody({ text: '😀'.repeat(2001), idempotencyKey: fixtures.requestId }));
  assert(!messageBody({ text: '\uD800', idempotencyKey: fixtures.requestId }));
  assert(!messageBody({ text: 'Hello', idempotencyKey: fixtures.requestId, upstreamUrl: 'http://localhost/admin' }));
});
