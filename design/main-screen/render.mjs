// Renders every mockup (*.html here) to out/<name>.png at 2560x1380 (2048x1104 DIPs at 125 %), like the real window.
// Usage: node render.mjs [name ...]   (Playwright from PW_DIR or the global install; missing skin art is fetched first)
// The browser is always closed in finally; no audio, headless.
import { createRequire } from 'node:module';
import { readdirSync, existsSync, mkdirSync, writeFileSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const req = createRequire(process.env.PW_DIR ? join(process.env.PW_DIR, 'x.js') : import.meta.url);
const { chromium } = req('playwright');

const SKINS = ['cid_084_athena_commando_m_assassin', 'cid_845_athena_commando_m_hightowertapas', 'cid_a_110_athena_commando_m_alientrooper'];
mkdirSync(join(here, 'art'), { recursive: true });
for (const s of SKINS) {
  const f = join(here, 'art', s + '.png');
  if (existsSync(f)) continue;
  const r = await fetch(`https://fortnite-api.com/images/cosmetics/br/${s}/featured.png`);
  if (!r.ok) throw new Error(`${s}: HTTP ${r.status}`);
  writeFileSync(f, Buffer.from(await r.arrayBuffer()));
}

const only = process.argv.slice(2);
const pages = readdirSync(here).filter((f) => f.endsWith('.html') && (!only.length || only.some((o) => f.startsWith(o))));
mkdirSync(join(here, 'out'), { recursive: true });
const browser = await chromium.launch({ headless: true, args: ['--mute-audio', '--allow-file-access-from-files'] });
try {
  for (const p of pages) {
    const ctx = await browser.newContext({ viewport: { width: 2048, height: 1104 }, deviceScaleFactor: 1.25 });
    try {
      const page = await ctx.newPage();
      await page.goto(pathToFileURL(join(here, p)).href, { waitUntil: 'load' });
      await page.evaluate(async () => {
        await document.fonts.ready;
        await Promise.all([...document.images].map((i) => (i.complete ? 0 : new Promise((r) => { i.onload = i.onerror = r; }))));
      });
      const broken = await page.evaluate(() => [...document.images].filter((i) => !i.naturalWidth).map((i) => i.src));
      const fonts = await page.evaluate(() => [...document.fonts].filter((f) => f.status === 'loaded').map((f) => f.family + ' ' + f.style));
      const out = join(here, 'out', p.replace(/\.html$/, '.png'));
      await page.screenshot({ path: out });
      await page.screenshot({ path: out.replace(/\.png$/, '.jpg'), type: 'jpeg', quality: 86 });   // light copy for the plan page
      console.log(JSON.stringify({ page: p, out, broken, fonts }));
      // a page can ask for more states: <body data-variants="settings"> adds body.settings and saves <name>-settings.png
      for (const v of await page.evaluate(() => (document.body.dataset.variants || '').split(' ').filter(Boolean))) {
        await page.evaluate((c) => document.body.classList.add(c), v);
        await page.screenshot({ path: out.replace(/\.png$/, `-${v}.png`) });
        await page.screenshot({ path: out.replace(/\.png$/, `-${v}.jpg`), type: 'jpeg', quality: 86 });
        await page.evaluate((c) => document.body.classList.remove(c), v);
      }
    } finally { await ctx.close(); }
  }
} finally { await browser.close(); }
