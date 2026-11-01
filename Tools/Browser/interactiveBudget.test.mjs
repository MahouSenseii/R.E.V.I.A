import test from 'node:test';
import assert from 'node:assert/strict';
import { createContext, runInContext } from 'node:vm';
import { InteractiveBrowser, validateOperation } from './interactiveHost.mjs';

const frameLimit = 262144;
const url = 'https://example.test/form';
const standard = { enabled: true, navigate: true, interact: true, allowLoopback: false,
  approvedOrigins: ['https://example.test'], timeoutMs: 15000,
  maxTextBytes: 8192, maxElements: 60, maxValueBytes: 4096 };
const maximum = { ...standard, maxTextBytes: 65536, maxElements: 200, maxValueBytes: 16384 };
const encodedBytes = result => Buffer.byteLength(`${JSON.stringify(result)}\n`, 'utf8');

// Substitute only the isolated-world evaluation transport. The production DOM description,
// signature, typed effect, observation and operate error paths still run.
function page({ grant = standard, count = 2, value = 'complete readable text', name = 'Shared field', text = 'Form' } = {}) {
  class Input {
    constructor() { this.stored = value; }
    get value() { return this.stored; }
    set value(next) { this.stored = next; }
    get tagName() { return 'INPUT'; }
    get type() { return 'text'; }
    get isConnected() { return true; }
    getAttribute(key) { return key === 'aria-label' ? name : null; }
    getBoundingClientRect() { return { width: 100, height: 20 }; }
    closest() { return null; }
    focus() {}
    dispatchEvent() {}
  }
  const elements = Array.from({ length: count }, () => new Input());
  const document = { title: 'Budget form', body: { innerText: text }, querySelectorAll: () => elements };
  const context = createContext({ document, location: { href: url }, TextEncoder,
    getComputedStyle: () => ({ visibility: 'visible', display: 'block' }),
    HTMLInputElement: Input, HTMLTextAreaElement: Input, Event: class {} });
  const browser = new InteractiveBrowser('unused-pure-budget-fixture');
  browser.session = 'budget-test-session';
  browser.grant = grant;
  browser.child = {}; // Already-owned sentinel: no process exists and start must never run.
  const forbidden = () => { throw new Error('Live browser, process, socket and DNS operations are forbidden.'); };
  browser.start = browser.send = browser.allowedNetwork = forbidden;
  browser.evaluate = async expression => {
    const result = runInContext(expression, context);
    return result === undefined ? undefined : JSON.parse(JSON.stringify(result));
  };
  const request = { operation: 'observe', url, grant };
  assert.equal(validateOperation(request, browser), '');
  return { browser, request, elements, document };
}

function assertRows(receipt, count, name, value) {
  assert.equal(receipt.elements.length, count, 'Budgeting must preserve every admitted target row.');
  for (let index = 0; index < count; index++) {
    const row = receipt.elements[index];
    assert.equal(row.id, `node-${index + 1}`);
    assert.equal(row.name, name);
    assert.equal(row.role, 'input');
    assert.equal(row.editable, true);
    assert.equal(row.clickable, true);
    assert.equal(typeof row.valueAvailable, 'boolean');
    assert.equal(row.value, row.valueAvailable ? value : '', 'A readback is complete or explicitly unavailable.');
  }
}

test('ordinary observations preserve complete readable values and mark over-bound values unavailable', async () => {
  const fixture = page({ value: 'Quotes " and \\ and 界🙂\nremain readable' });
  fixture.elements[1].value = 'x'.repeat(4097);
  const result = await fixture.browser.operate(fixture.request);
  assert.equal(result.succeeded, true);
  assert.equal(result.attempted, false);
  assert.ok(encodedBytes(result) <= frameLimit);
  assert.deepEqual(result.receipt.elements[0], { id: 'node-1', name: 'Shared field', role: 'input',
    clickable: true, editable: true, valueAvailable: true, value: 'Quotes " and \\ and 界🙂\nremain readable' });
  assert.equal(result.receipt.elements[1].valueAvailable, false);
  assert.equal(result.receipt.elements[1].value, '');
  assert.equal(result.receipt.text, 'Form');
  assert.equal(result.receipt.title, 'Budget form');
});

// Missing encoded-frame budgeting, raw-string accounting, available truncation and row
// deletion each violate these independently checked wire/target/readback contracts.
for (const [label, grant, value] of [
  ['standard quoted values', standard, '"'.repeat(4096)],
  ['standard backslash values', standard, '\\'.repeat(4096)],
  ['standard control values', standard, '\u0001'.repeat(4096)],
  ['maximum escaped values', maximum, '"\\\u0001\n'.repeat(4096)],
  ['maximum multibyte values', maximum, '界🙂'.repeat(2340)]
]) {
  test(`${label} fit the full UTF-8 frame without losing duplicate field identities`, async () => {
    const fixture = page({ grant, count: grant.maxElements, value });
    const result = await fixture.browser.operate(fixture.request);
    assert.equal(result.succeeded, true, result.message);
    assert.ok(encodedBytes(result) <= frameLimit, `Encoded response is ${encodedBytes(result)} bytes.`);
    assertRows(result.receipt, grant.maxElements, 'Shared field', value);
    const unavailable = result.receipt.elements.filter(row => !row.valueAvailable).map(row => row.id);
    assert.ok(unavailable.length > 0, 'Oversized readback must become unavailable.');
    assert.match(result.message, /omitt|unavailable|readback/i, 'Omission must be visible to the consumer.');
    const again = await fixture.browser.operate(fixture.request);
    assert.equal(again.succeeded, true, again.message);
    assert.ok(encodedBytes(again) <= frameLimit);
    assertRows(again.receipt, grant.maxElements, 'Shared field', value);
    assert.deepEqual(again.receipt.elements.filter(row => !row.valueAvailable).map(row => row.id), unavailable);
    assert.equal(again.receipt.generation, result.receipt.generation + 1);
    assert.equal(again.receipt.fingerprint, result.receipt.fingerprint);
    fixture.elements[Number(unavailable[0].slice(5)) - 1].value = 'changed';
    const changed = await fixture.browser.observe();
    assert.notEqual(changed.fingerprint, again.receipt.fingerprint, 'Omitted values still participate in the DOM fingerprint.');
  });
}

// An independent literal envelope sizes essential metadata at the native boundary.
// Its fingerprint contents are irrelevant: SHA-256 hex always contributes 64 ASCII bytes.
function essentialFixture(extraByte = 0) {
  const name = '"'.repeat(180);
  const elements = Array.from({ length: 200 }, (_, index) => ({ id: `node-${index + 1}`, name, role: 'input',
    clickable: true, editable: true, valueAvailable: false, value: '' }));
  const envelope = { succeeded: true, attempted: false, message: 'Current browser state observed.', receipt: {
    session: 'budget-test-session', generation: 1, url, title: 'Budget form', text: '', elements, fingerprint: '0'.repeat(64) } };
  const paddingBytes = frameLimit - encodedBytes(envelope) + extraByte;
  const text = '\u0001'.repeat(Math.floor(paddingBytes / 6)) + 'x'.repeat(paddingBytes % 6);
  assert.ok(Buffer.byteLength(text) <= maximum.maxTextBytes);
  return page({ grant: maximum, count: 200, value: 'x'.repeat(16385), name, text });
}

test('essential metadata exactly at the full frame boundary remains observable', async () => {
  const { browser, request } = essentialFixture();
  const result = await browser.operate(request);
  assert.equal(result.succeeded, true, result.message);
  assert.equal(encodedBytes(result), frameLimit);
  assertRows(result.receipt, 200, '"'.repeat(180), '');
});

test('essential metadata one byte beyond the full frame boundary fails before an effect and invalidates targets', async () => {
  const { browser, request } = essentialFixture(1);
  const result = await browser.operate(request);
  assert.equal(result.succeeded, false, 'Receipt-only accounting misses the envelope and final newline.');
  assert.equal(result.attempted, false);
  assert.equal(result.uncertainEffect, false);
  assert.equal(result.receipt, undefined);
  assert.equal(browser.last, null);
  assert.ok(browser.generation > 1);
  assert.ok(encodedBytes(result) <= frameLimit);
  assert.match(result.message, /bound|budget|size|large|limit/i);
});

test('unfittable observation after a typed effect reports attempted uncertainty and invalidates targets', async () => {
  const fixture = page({ grant: maximum });
  const initial = await fixture.browser.operate(fixture.request);
  assert.equal(initial.succeeded, true);
  const request = { operation: 'fill', url, grant: maximum, session: initial.receipt.session,
    generation: initial.receipt.generation, element: 'node-1', value: 'effect applied' };
  // Keep the pre-effect DOM signature valid; expand the observed page only when input fires.
  fixture.elements[0].dispatchEvent = () => { fixture.document.body.innerText = '\u0001'.repeat(65536); };
  const result = await fixture.browser.operate(request);
  assert.equal(result.succeeded, false);
  assert.equal(result.attempted, true);
  assert.equal(result.uncertainEffect, true);
  assert.equal(result.receipt, undefined);
  assert.equal(fixture.browser.last, null);
  assert.ok(fixture.browser.generation > initial.receipt.generation);
  assert.ok(encodedBytes(result) <= frameLimit);
  assert.match(result.message, /bound|budget|size|large|limit/i);
});
