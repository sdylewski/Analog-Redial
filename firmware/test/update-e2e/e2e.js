#!/usr/bin/env node
/* End-to-end test of Settings -> Hardware -> Firmware "Check for updates" / "Install", in a real
 * (headless) Chrome against a real board. It serves its own fake GitHub (a manifest and images,
 * with CORS) from this computer and points the page at it through localStorage.arFwUrl, so it
 * needs neither GitHub nor a public repository.
 *
 *   cd firmware/test/update-e2e && npm install
 *   node e2e.js --board 192.168.0.170 --good ../../release/out/v0.0.1/smd5-16MB.bin \
 *               --wrong ../../build/rotary_phone_gateway.bin
 *
 *   --good   a firmware image built for THIS board (its id is read from the file)
 *   --wrong  an image built for a different board type (it must be refused)
 *   --chrome path to Chrome (default: macOS location)
 *
 * It really installs --good (the board reboots into it) -- use a bench board.
 * Run it from a normal terminal: on macOS, programs started by some tools are not allowed to
 * reach devices on the local network. */
const http = require('http'), fs = require('fs'), os = require('os'), path = require('path');
const { chromium } = require('playwright-core');

const arg = (n, d) => { const i = process.argv.indexOf('--' + n); return i > 0 ? process.argv[i + 1] : d; };
const BOARD = 'http://' + arg('board');
const GOOD = arg('good'), WRONG = arg('wrong');
const CHROME = arg('chrome', '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome');
if (!arg('board') || !GOOD || !WRONG) { console.log(require('fs').readFileSync(__filename, 'utf8').split('*/')[0]); process.exit(1); }
const sleep = ms => new Promise(r => setTimeout(r, ms));

const lanIp = Object.values(os.networkInterfaces()).flat().find(i => i.family === 'IPv4' && !i.internal).address;
const idOf = f => { const m = /\x01AR-BOARD\x02([A-Za-z0-9_\/]+)\x03/.exec(fs.readFileSync(f).toString('latin1')); return m && m[1]; };
const goodId = idOf(GOOD);
if (!goodId) throw new Error(GOOD + ' has no board id');
const files = { 'good.bin': GOOD, 'wrong.bin': WRONG };
const manifest = (version, file, size) => JSON.stringify({
  version, released: '2026-01-01', notes: 'End-to-end test release', base: `http://${lanIp}:8765/`,
  images: { [goodId]: { file, size } } });
const size = f => fs.statSync(f).size;
const docs = {
  '/latest.json': manifest('v9.9.9', 'good.bin', size(GOOD)),
  '/wrongboard.json': manifest('v9.9.9', 'wrong.bin', size(WRONG)),
  '/badsize.json': manifest('v9.9.9', 'good.bin', size(GOOD) + 1000),
};
const server = http.createServer((req, res) => {
  res.setHeader('Access-Control-Allow-Origin', '*'); res.setHeader('Cache-Control', 'no-store');
  const name = req.url.slice(1);
  if (docs[req.url]) { res.setHeader('Content-Type', 'application/json'); return res.end(docs[req.url]); }
  if (files[name]) return res.end(fs.readFileSync(files[name]));
  res.statusCode = 404; res.end('no');
}).listen(8765, '0.0.0.0');

async function status() { const r = await fetch(BOARD + '/api/status'); return r.json(); }
async function waitBoard() { for (let i = 0; i < 60; i++) { try { return (await status()).fw_version; } catch (e) { await sleep(3000); } } }

(async () => {
  const st = await status();
  if (st.board_image !== goodId) throw new Error(`board is ${st.board_image} but --good is for ${goodId}`);
  const goodVer = (/\x00(v\d+\.\d+\.\d+)[^\x00]*\x00/.exec(fs.readFileSync(GOOD).toString('latin1')) || [])[1];
  console.log(`board ${BOARD} runs ${st.fw_version} (${st.board_image}); serving fake releases at http://${lanIp}:8765/`);
  const browser = await chromium.launch({ executablePath: CHROME, headless: true });
  const results = [];
  async function scenario(name, manifestFile, fn) {
    const ctx = await browser.newContext(); const page = await ctx.newPage();
    page.on('dialog', d => d.accept());
    await page.addInitScript(u => { localStorage.setItem('arFwUrl', u); localStorage.removeItem('arFwSeen'); }, `http://${lanIp}:8765/${manifestFile}`);
    try { results.push([name, 'ok', await fn(page)]); }
    catch (e) { results.push([name, 'FAIL', String(e).split('\n')[0]]); }
    await ctx.close();
  }
  const hw = async page => { await page.goto(BOARD + '/settings?tab=hardware'); await page.waitForSelector('#fwinstall', { state: 'visible', timeout: 15000 }); };
  const failed = page => page.waitForFunction(() => /^Failed/.test(document.getElementById('fwstat').textContent), null, { timeout: 90000 });

  await scenario('banner on the status page', 'latest.json', async page => {
    await page.goto(BOARD + '/'); await page.waitForSelector('#fwnotice', { state: 'visible', timeout: 15000 });
    return (await page.textContent('#fwnotice')).trim();
  });
  await scenario('"Not now" hides the banner', 'latest.json', async page => {
    await page.goto(BOARD + '/'); await page.waitForSelector('#fwnotice', { state: 'visible', timeout: 15000 });
    await page.click('#fwdismiss'); await sleep(300);
    return 'hidden: ' + !(await page.isVisible('#fwnotice'));
  });
  await scenario('image for another board is refused', 'wrongboard.json', async page => {
    await hw(page); await page.click('#fwinstall'); await failed(page);
    return (await page.textContent('#fwstat')) + ' | board still runs ' + (await status()).fw_version;
  });
  await scenario('short download is refused', 'badsize.json', async page => {
    await hw(page); await page.click('#fwinstall'); await failed(page);
    return await page.textContent('#fwstat');
  });
  await scenario('install a good update', 'latest.json', async page => {
    await hw(page); const t0 = Date.now(); await page.click('#fwinstall');
    await page.waitForFunction(() => /^(Updated|The board)/.test(document.getElementById('fwstat').textContent), null, { timeout: 240000 });
    return `${await page.textContent('#fwstat')} (${Math.round((Date.now() - t0) / 1000)} s); board now runs ${await waitBoard()}`;
  });
  await browser.close(); server.close();
  let bad = 0;
  for (const r of results) { if (r[1] !== 'ok') bad++; console.log(r[1].padEnd(5), r[0].padEnd(36), r[2]); }
  process.exit(bad ? 1 : 0);
})();
