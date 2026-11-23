// SPDX-License-Identifier: AGPL-3.0-or-later
// web.js URL STATUS_FILE OUT_DIR: the four pages every theme is shown with, as a
// browser on a desk and one on a mobile phone, each light and dark, at twice the size.
const { chromium } = require('playwright');
const fs = require('fs');
const [url, statusFile, out] = process.argv.slice(2);

(async () => {
  fs.mkdirSync(out, { recursive: true });
  const browser = await chromium.launch(process.env.CHROMIUM ? { executablePath: process.env.CHROMIUM } : {});
  const first = await browser.newContext();
  const page = await first.newPage();
  await page.goto(url);
  await page.click('text=Show code');
  await page.waitForURL('**/pair');
  const code = /pair_code=(\d+)/.exec(fs.readFileSync(statusFile, 'utf8'))[1];
  await page.fill('input[name=code]', code);
  await page.click('button:has-text("Pair")');
  await page.waitForURL(url + '/');
  const storageState = await first.storageState();
  for (const scheme of ['dark', 'light']) {
    for (const [prefix, viewport] of [['web', { width: 1180, height: 820 }], ['web-mobile', { width: 390, height: 780 }]]) {
      const ctx = await browser.newContext({ viewport, deviceScaleFactor: 2, colorScheme: scheme, storageState });
      const p = await ctx.newPage();
      for (const [path, name] of [['/', 'overview'], ['/accounts', 'accounts'], ['/backup', 'backup'], ['/sound', 'sound']]) {
        await p.goto(url + path);
        await p.evaluate(() => document.fonts.ready);
        await p.screenshot({ path: `${out}/${prefix}-${name}-${scheme}.png`, fullPage: name === 'sound' });
      }
      await ctx.close();
    }
  }
  await browser.close();
})().catch((e) => { console.error(e); process.exit(1); });
