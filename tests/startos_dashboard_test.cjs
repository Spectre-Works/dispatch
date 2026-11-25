// SPDX-FileCopyrightText: 2026 SPECTRE WORKS LLC
// SPDX-License-Identifier: LicenseRef-Spectre-Dispatch-1.0
const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const { chromium } = require('playwright');

async function check() {
  const [url, keyFile, exportPath, mode] = process.argv.slice(2);
  const browser = await chromium.launch({ executablePath: process.env.DISPATCH_TEST_CHROMIUM });
  try {
    const page = await browser.newPage({ acceptDownloads: true });
    const session = await page.context().newCDPSession(page);
    await session.send('Page.setFontSizes', { fontSizes: { standard: 20 } });
    const errors = [];
    page.on('pageerror', error => errors.push(error.message));
    await page.goto(url);
    await page.locator('#owner-key').fill(await fs.readFile(keyFile, 'utf8'));
    await page.locator('#unlock').click();
    await page.locator('#dashboard').waitFor({ state: 'visible' });
    assert.equal(await page.locator('html').evaluate(root => getComputedStyle(root).fontSize), '16px');
    const mark = page.locator('.wordmark img');
    await mark.evaluate(img => img.decode());
    assert.equal(await page.getByRole('heading', { name: 'SPECTRE WORKS DISPATCH', level: 1 }).count(), 1);
    assert.match(await mark.getAttribute('src'), /^data:image\/svg\+xml;base64,/);
    assert.match(await page.locator('#mode').textContent(), /Preview/);
    assert.equal(await page.locator('#control').isVisible(), false);
    await page.locator('[data-view="settings"]').click();
    assert.equal(await page.locator('#discover').isVisible(), false);
    await page.locator('[data-settings="transfer-settings"]').click();
    if (mode === 'import') {
      await page.locator('#import-file').setInputFiles(exportPath);
      await page.locator('#import-preview').waitFor({ state: 'visible' });
      const saved = page.waitForResponse(response => new URL(response.url()).pathname === '/api/import');
      await page.locator('#apply-import').click();
      assert.equal((await saved).status(), 200);
      await page.locator('#import-preview').waitFor({ state: 'hidden' });
      const status = await page.evaluate(async owner => {
        const response = await fetch('/api/status', { headers: { 'X-Dispatch-Key': owner } });
        return response.json();
      }, await fs.readFile(keyFile, 'utf8'));
      assert.equal(status.enabled, false);
      assert.equal(status.rate, 12.345);
      assert.equal(status.devices[0].identity, 'shellyplusplugsus-000000000001');
      assert.equal(status.devices[0].firstRun.mapping, false);
      assert.equal(status.mqtt.ip, '');
      assert.equal(status.home.ip, '');
      assert.deepEqual(errors, []);
      return;
    }
    const downloaded = page.waitForEvent('download');
    await page.locator('#export-config').click();
    await (await downloaded).saveAs(exportPath);
    const plan = JSON.parse(await fs.readFile(exportPath, 'utf8'));
    assert.equal(plan.format, 'dispatch-configuration');
    assert.equal(plan.version, 4);
    assert.equal(plan.rate, 12345);
    assert.equal(plan.equipment[0].identity, 'shellyplusplugsus-000000000001');
    assert.equal(plan.equipment[0].minerIp, '10.241.0.1');
    assert.equal(plan.equipment.length, 8);
    assert.deepEqual(Object.keys(plan.equipment[0]).sort(), ['identity', 'ip', 'minerIp', 'slot']);
    assert.equal(JSON.stringify(plan).includes('fixturepass'), false);
    assert.equal(JSON.stringify(plan).includes('owner'), false);
    await page.getByRole('button', { name: 'Activity', exact: true }).click();
    await page.locator('#events').getByText(/Owner cleared electricity accounting/).waitFor();
    await page.waitForFunction(() => activityData?.range === activityRange && activityPoints().length >= 3);
    const scrub = page.locator('#activity-scrub');
    const explore = index => scrub.evaluate((slider, value) => {
      slider.value = value;
      slider.dispatchEvent(new Event('input', { bubbles: true }));
    }, index);
    // Inject boundary timestamps into the loaded history to exercise the real slider and card.
    await page.evaluate(() => {
      const points = activityPoints();
      activityData.events = [
        { at: points[0].at, label: 'At selected start', slot: 8, id: 0, lane: 'bridge' },
        { at: points[1].at, label: 'At selected end', slot: 8, id: 0, lane: 'bridge' },
      ];
    });
    const selectedEvents = page.locator('#activity-events li');
    await explore(0);
    assert.equal(await selectedEvents.count(), 1);
    assert.match(await selectedEvents.first().textContent(), /At selected start/);
    await explore(1);
    assert.equal(await selectedEvents.count(), 1);
    assert.match(await selectedEvents.first().textContent(), /At selected end/);
    await explore(2);
    assert.equal(await selectedEvents.count(), 0);
    const clear = page.getByRole('button', { name: 'Clear activity history' });
    const canceled = page.waitForEvent('dialog');
    const firstClick = clear.click();
    const confirmation = await canceled;
    assert.match(confirmation.message(), /operating plan, equipment pairings, and settings will be kept/);
    await confirmation.dismiss();
    await firstClick;
    assert.ok(await page.locator('#events').getByText(/Owner cleared electricity accounting/).isVisible());
    page.once('dialog', dialog => dialog.accept());
    await clear.click();
    await page.getByText('Activity history cleared. Your settings were kept.').waitFor();
    assert.equal(await page.getByText(/Owner cleared electricity accounting/).count(), 0);
    assert.deepEqual(errors, []);
  } finally {
    await browser.close();
  }
}

check().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
