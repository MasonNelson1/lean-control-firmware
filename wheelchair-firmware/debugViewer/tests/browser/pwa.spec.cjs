const {test, expect, chromium} = require('@playwright/test');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const mockSerial = require('./mock-serial.cjs');

test('hosted subpath loads with logo and no Python or third-party requests', async ({page}) => {
  const failures = [], errors = [], requests = [];
  page.on('response', response => { if (response.status() >= 400) failures.push(response.url()); });
  page.on('pageerror', error => errors.push(error.message));
  page.on('websocket', socket => requests.push(socket.url()));
  page.on('request', request => requests.push(request.url()));
  await page.goto('./');
  await expect(page.locator('#connection-mode')).toHaveValue('bluetooth');
  await expect(page.locator('#connection-mode option')).toHaveCount(1);
  await expect(page.locator('#connection-connect')).toBeEnabled();
  await expect(page.locator('#status')).toHaveText('Bluetooth disconnected');
  expect(await page.locator('.brand-logo').evaluate(image => image.complete && image.naturalWidth > 0)).toBe(true);
  await expect(page.locator('.brand-logo')).toHaveAttribute('src', './assets/lean-full.svg');
  await expect(page.locator('link[rel="icon"]')).toHaveAttribute('href', './assets/lean-wheelchair.svg');
  expect(await page.locator('.brand-logo').evaluate(image => {
    const canvas = document.createElement('canvas');
    canvas.width = image.naturalWidth; canvas.height = image.naturalHeight;
    const context = canvas.getContext('2d');
    context.drawImage(image, 0, 0);
    return context.getImageData(0, 0, 1, 1).data[3];
  })).toBe(0);
  const favicon = await page.request.get('assets/lean-wheelchair.svg');
  expect(favicon.headers()['content-type']).toBe('image/svg+xml');
  await page.screenshot({path: test.info().outputPath('dashboard-logo.png')});
  await page.waitForTimeout(500);
  expect(failures).toEqual([]);
  expect(errors).toEqual([]);
  expect(requests.every(url => url.startsWith('http://127.0.0.1:8123/lean-control-firmware/'))).toBe(true);
  expect(requests.some(url => /\/profiles\/|\/ws$|\/bluetooth\/disconnect/.test(url))).toBe(false);
});

test('manifest meets Chrome installability requirements outside incognito', async ({baseURL}) => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'lean-installability-'));
  const context = await chromium.launchPersistentContext(directory, {
    headless: true, ...(process.env.LEAN_BROWSER_CHANNEL ? {channel: process.env.LEAN_BROWSER_CHANNEL} : {})
  });
  try {
    const page = await context.newPage();
    await page.goto(baseURL);
    const session = await context.newCDPSession(page);
    const manifest = await session.send('Page.getAppManifest');
    expect(manifest.errors).toEqual([]);
    expect(JSON.parse(manifest.data).name).toBe('Lean Debug Viewer');
    const installability = await session.send('Page.getInstallabilityErrors');
    expect(installability.installabilityErrors).toEqual([]);
  } finally {
    await context.close();
    fs.rmSync(directory, {recursive: true, force: true});
  }
});

test('the cached application reopens offline', async ({page, context}) => {
  await page.goto('./');
  await page.evaluate(async () => { await navigator.serviceWorker.ready; });
  // First activation does not claim open windows. The next navigation is controlled.
  await page.reload();
  await expect.poll(() => page.evaluate(() => !!navigator.serviceWorker.controller)).toBe(true);
  await context.setOffline(true);
  await page.reload();
  await expect(page.locator('#connection-connect')).toBeEnabled();
  expect(await page.locator('.brand-logo').evaluate(image => image.complete && image.naturalWidth > 0)).toBe(true);
  // Test profile encoding/decoding while offline; no serial device is required.
  const profile = await page.evaluate(async () => LeanProfiles.decode(await LeanProfiles.encode({
    format: 'lean-config', version: 1, fields: [{id: 1, type: 10, value: '18446744073709551615'}]
  })));
  expect(profile.fields[0].value).toBe('18446744073709551615');
});

test('updates wait until old application windows close', async ({page, context}) => {
  await page.goto('./');
  await page.evaluate(async () => { await navigator.serviceWorker.ready; });
  await page.reload();
  const original = await page.evaluate(() => navigator.serviceWorker.controller.scriptURL);
  // The preview server changes only its temporary build, never source files.
  await page.request.post('../__test-update');
  await page.evaluate(async () => { const registration = await navigator.serviceWorker.getRegistration(); await registration.update(); });
  await expect.poll(() => page.evaluate(async () => !!(await navigator.serviceWorker.getRegistration()).waiting)).toBe(true);
  expect(await page.evaluate(() => navigator.serviceWorker.controller.scriptURL)).toBe(original);
  expect(await page.evaluate(async () => (await navigator.serviceWorker.getRegistration()).active.state)).toBe('activated');
  await page.close();
  const next = await context.newPage();
  await next.goto('./');
  await expect.poll(() => next.evaluate(async () => !(await navigator.serviceWorker.getRegistration()).waiting)).toBe(true);
  expect(await next.evaluate(() => caches.keys())).toEqual([expect.stringContaining('update1-')]);
});

test('Bluetooth settings and profiles work without a server and retain edit highlights', async ({page}) => {
  await mockSerial(page);
  const requests = [];
  page.on('request', request => requests.push(request.url()));
  await page.goto('./');
  await page.locator('#connection-connect').click();
  await page.locator('[data-page="configuration"]').click();
  await expect(page.locator('#field-10')).toHaveValue('2');
  await page.locator('#field-10').fill('4.25');
  await expect(page.locator('#config-field-10')).toHaveClass(/config-field-pending/);
  expect(await page.evaluate(() => serialTestCommands.includes(11))).toBe(false);
  await page.evaluate(() => serialTestMode('DISARM'));
  page.once('dialog', dialog => dialog.accept('Rider'));
  await page.locator('#config-profile-save').click();
  await expect(page.locator('#config-profiles option').first()).toHaveText('1. Rider');
  await page.getByRole('button', {name: 'Reset Front gain to current value', exact: true}).click();
  await expect(page.locator('#config-field-10')).not.toHaveClass(/config-field-pending/);
  await page.evaluate(() => serialTestMode('ASSIST'));
  await expect(page.locator('#config-apply')).toBeDisabled();
  await page.locator('#config-profile-load').click();
  await expect(page.locator('#field-10')).toHaveValue('4.25');
  await expect(page.locator('#config-field-10')).toHaveClass(/config-field-pending/);
  expect(await page.evaluate(() => serialTestCommands.includes(5))).toBe(false);
  await page.evaluate(() => serialTestMode('DISARM'));
  await page.locator('#config-apply').click();
  await expect(page.locator('#config-field-10')).not.toHaveClass(/config-field-pending/);
  expect(await page.evaluate(() => serialTestCommands.includes(5))).toBe(true);
  expect(requests.some(url => /\/profiles\/|\/bluetooth\/disconnect/.test(url))).toBe(false);
  await page.locator('#connection-disconnect').click();
  await expect(page.locator('#connection-connect')).toBeEnabled();
});

test('local Python mode preserves USB/replay and does not register a service worker', async ({page}) => {
  await page.goto('http://127.0.0.1:8124');
  await expect(page.locator('#connection-mode')).toHaveValue('bluetooth');
  await expect(page.locator('#connection-mode option')).toHaveCount(2);
  await expect(page.locator('#status')).toContainText('Bluetooth');
  expect(await page.evaluate(() => dashboardBackend)).toBe(true);
  expect(await page.evaluate(async () => (await navigator.serviceWorker.getRegistrations()).length)).toBe(0);
  expect(await page.locator('.brand-logo').evaluate(image => image.complete && image.naturalWidth > 0)).toBe(true);
});

test('live gain status appears beside sensor ADC counts and clears on reconnect', async ({page}) => {
  const errors = [];
  page.on('pageerror', error => errors.push(error.message));
  await mockSerial(page);
  await page.goto('./');
  await expect(page.locator('.sensor.front .gain')).toHaveText('—');
  await page.locator('#connection-connect').click();
  await page.evaluate(() => serialTestLine('DYNAMIC_GAINS F=1.2300 B=2.3400 L=1.0000 R=0.9800'));
  await expect(page.locator('.sensor.front strong')).toHaveText('100');
  await expect(page.locator('.sensor.front .gain')).toHaveText('×1.23');
  await expect(page.locator('.sensor.back .gain')).toHaveText('×2.34');
  await expect(page.locator('.sensor.left .gain')).toHaveText('×1.00');
  await expect(page.locator('.sensor.right .gain')).toHaveText('×0.98');
  await expect(page.locator('pre')).not.toContainText('DYNAMIC_GAINS');
  await page.screenshot({path: test.info().outputPath('dynamic-gains.png')});
  await page.locator('#connection-disconnect').click();
  await expect(page.locator('.sensor.front .gain')).toHaveText('—');
  await page.locator('#connection-connect').click();
  await expect(page.locator('.sensor.front .gain')).toHaveText('—');
  await page.evaluate(() => serialTestLine('DYNAMIC_GAINS F=5.0000 B=4.0000 L=3.0000 R=2.0000'));
  await expect(page.locator('.sensor.front .gain')).toHaveText('×5.00');
  expect(errors).toEqual([]);
  await page.locator('#connection-disconnect').click();
});
