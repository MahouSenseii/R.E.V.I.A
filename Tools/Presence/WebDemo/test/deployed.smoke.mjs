// Explicit operator-run test. No relay/native/model responses are intercepted.
// Only this browser's public configuration is overridden, so published chat can
// remain disabled until the real deployment passes. Never runs in npm test/CI.
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { loadSettings, readStatus } from '../operator/config.js';

async function main() {
const directory = fileURLToPath(new URL('..', import.meta.url));
const config = await loadSettings(process.env.REVIA_WEB_CONFIG_DIR || directory, true);
const invite = process.env.REVIA_WEB_TEST_INVITE;
assert.ok(typeof invite === 'string' && invite.length >= 32 && invite.length <= 256, 'Set REVIA_WEB_TEST_INVITE privately; do not put it on the command line.');
const portfolio = process.env.REVIA_WEB_PORTFOLIO_ROOT || resolve(directory, '../../../../Portfolio');
const { chromium } = createRequire(resolve(portfolio, 'package.json'))('@playwright/test');
const pageUrl = 'https://mahousenseii.github.io/Portfolio/#revia.html';
assert.equal(await readStatus(config.localUrl + '/web/v1/status', config.token), 'online', 'Native must be online on this PC.');
assert.equal(await readStatus(config.relayOrigin + '/v1/status'), 'online', 'Public relay must report online.');
const browser = await chromium.launch({ headless: true });
const contexts = [];
try {
  for (let guest = 0; guest < 2; guest++) {
    const context = await browser.newContext({ serviceWorkers: 'block' }); contexts.push(context);
    const page = await context.newPage();
    await page.route('https://mahousenseii.github.io/Portfolio/data/revia-demo.json', route => route.fulfill({ json: { enabled: true, relayUrl: config.relayOrigin } }));
    await page.goto(pageUrl);
    await page.getByLabel('Invitation code').fill(invite);
    await page.getByRole('button', { name: 'Start session', exact: true }).click();
    await page.getByLabel('Your message').fill(guest === 0 ? 'Hi Revia! Describe a maple leaf in one short sentence.' : 'Hi Revia! Describe a snowflake in one short sentence.');
    const accepted = page.waitForResponse(r => r.url() === config.relayOrigin + '/v1/messages' && r.request().method() === 'POST');
    await page.getByRole('button', { name: 'Send message', exact: true }).click();
    const submitted = await accepted;
    assert.equal(submitted.status(), 202);
    const reply = page.locator('[data-transcript] [data-speaker="Revia"] p');
    await reply.waitFor({ state: 'visible', timeout: 125000 });
    assert.ok((await reply.textContent()).trim(), 'Expected a real completed reply.');
    const ended = page.waitForResponse(r => r.url() === config.relayOrigin + '/v1/session' && r.request().method() === 'DELETE');
    await page.getByRole('button', { name: 'End session & clear', exact: true }).click();
    assert.equal((await ended).status(), 204);
    console.log(`Deployed browser guest ${guest + 1}: real reply received and session deletion acknowledged.`);
  }
  console.log('PASS: published Portfolio browser -> HTTPS relay -> connected bridge -> native/local model returned replies. Published configuration is unchanged.');
  console.log('Also perform the documented cancel, pause, disconnect and isolation checks before public activation.');
} finally {
  // pagehide invokes the real client cleanup on partial failures, too.
  for (const context of contexts) {
    for (const page of context.pages()) await page.goto('about:blank').catch(() => {});
    await context.close();
  }
  await browser.close();
}
}

// Playwright error call logs can include fill() arguments. Do not print raw
// exceptions: the invite must never appear in a failed browser action's log.
try { await main(); }
catch {
  console.error('Deployed smoke failed. Check private configuration, invitation, published page, Chromium installation, native/relay readiness and the full deployed route. No successful deployment is claimed.');
  process.exitCode = 1;
}
