// SPDX-License-Identifier: AGPL-3.0-or-later
// shots.js URL STATUS_FILE OUT_DIR: pictures of every page, as a browser on a
// desk and as one on a mobile phone. Pairs the way a person does: asks for a
// code, reads it where the phone's screen reads it from, types it in.
const { chromium } = require('playwright');
const fs = require('fs');
const [url, statusFile, out] = process.argv.slice(2);

(async () => {
  fs.mkdirSync(out, { recursive: true });
  const browser = await chromium.launch(process.env.CHROMIUM ? { executablePath: process.env.CHROMIUM } : {});
  const ctx = await browser.newContext({ viewport: { width: 1180, height: 820 }, deviceScaleFactor: 1 });
  const page = await ctx.newPage();
  const shot = (name, full) => page.screenshot({ path: `${out}/web-${name}.png`, fullPage: !!full });

  await page.goto(url);
  await shot('welcome');
  await page.click('text=Show code');
  await page.waitForURL('**/pair');
  const code = /pair_code=(\d+)/.exec(fs.readFileSync(statusFile, 'utf8'))[1];
  await page.fill('input[name=code]', code.slice(0, 3) + ' ' + code.slice(3));
  await shot('pair');
  // a picture with a real code in it is harmless: it was good for two minutes, once
  await page.click('button:has-text("Pair")');
  await page.waitForURL(url + '/');
  await shot('overview');

  for (const [path, name] of [['/accounts', 'accounts'], ['/network', 'network'], ['/time', 'time'], ['/sound', 'sound'],
                              ['/contacts', 'contacts'], ['/history', 'history'], ['/apps', 'apps'], ['/access', 'access'], ['/backup', 'backup']]) {
    await page.goto(url + path);
    if (name === 'apps') {
      await page.click('details.sub summary');
      // the test feeds are folders on this computer: shown under made-up web addresses instead,
      // as SHOT_REPLACE lists them in pairs (shots.sh)
      const pairs = JSON.parse(process.env.SHOT_REPLACE || '[]');
      await page.evaluate((pairs) => {
        const walk = document.createTreeWalker(document.body, NodeFilter.SHOW_TEXT);
        for (let n; (n = walk.nextNode());)
          for (const [from, to] of pairs) n.nodeValue = n.nodeValue.split(from).join(to);
      }, pairs);
    }
    await shot(name, true);
  }
  // Accounts, Advanced: the dial plan, open
  await page.goto(url + '/accounts');
  await page.$eval('#advanced', (d) => { d.open = true; });   // open already when certificates are kept
  await page.locator('#advanced').screenshot({ path: `${out}/web-accounts-advanced.png` });
  // its certificates for SIP over TLS (issue #78)
  await page.locator('#certs').screenshot({ path: `${out}/web-certs.png` });
  // a change, and what the page says about it
  await page.goto(url + '/contacts');
  await page.fill('input[name=name]', 'Workshop');
  await page.fill('input[name=number]', '2002');
  await page.click('button:has-text("Add")');
  await page.waitForSelector('.flash');
  await shot('contacts-saved');

  // the same on a mobile phone
  const small = await browser.newContext({ viewport: { width: 390, height: 780 }, deviceScaleFactor: 1, storageState: await ctx.storageState() });
  const p2 = await small.newPage();
  for (const [path, name] of [['/', 'mobile-overview'], ['/accounts', 'mobile-accounts'], ['/history', 'mobile-history']]) {
    await p2.goto(url + path);
    await p2.screenshot({ path: `${out}/web-${name}.png` });
  }
  await browser.close();
})().catch((e) => { console.error(e); process.exit(1); });
