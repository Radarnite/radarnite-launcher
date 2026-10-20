// The native-only selftest scenarios (20-23 of native/ui/mock_api.cpp) rendered by the page, so they can be compared
// too: tests/ui-scenarios.mjs never sets matchServer, so the bots form, switches, stepper and slider only show here.
// usage: node page-extra.mjs <ui dir> <out dir>   (PW_DIR = folder with node_modules/playwright; headless Chromium)
import { createRequire } from 'node:module';
import path from 'node:path';
import fs from 'node:fs';
const require = createRequire(process.env.PW_DIR || import.meta.url);
const { chromium } = require('playwright');
const [ui, out] = process.argv.slice(2);
fs.mkdirSync(out, { recursive: true });

// same fixtures as tests/ui-scenarios.mjs, with the match server present
const b1750 = { name: '17.50', dir: 'S:\\FortniteBuilds\\17.50', version: '17.50', major: 17, minor: 50, cl: 17388565, engine: '4.26', lobby: 'Yes', lobbyWhy: 'UE4 (3.x-19.x): curl redirect + -noeac launch', match: 'Experimental', matchWhy: "Reboot: S16-S19 and S1-S2 'might not work perfectly'", reboot: true, cosmetics: 5338, firewall: 'OK', matchServer: true };
const mk = (v, maj, min, cl, eng, extra = {}) => ({ ...b1750, name: v, dir: `S:\\FortniteBuilds\\${v}`, version: v, major: maj, minor: min, cl, engine: eng, ...extra });
const many = [
  mk('3.5', 3, 5, 4166199, '4.20', { match: 'Yes', matchWhy: 'Reboot: S3-S15 supported', cosmetics: 412 }),
  mk('7.40', 7, 40, 5046157, '4.22', { match: 'Yes', matchWhy: 'Reboot: S3-S15 supported', cosmetics: 1530 }),
  mk('12.41', 12, 41, 12905909, '4.24', { match: 'Yes', matchWhy: 'Reboot: S3-S15 supported', cosmetics: 0, firewall: 'firewall rules missing for 9 programs (e.g. FortniteLauncher.exe)' }),
  b1750,
  mk('21.10', 21, 10, 21543261, '5.1', { lobby: 'Experimental', lobbyWhy: 'UE5 (20.x-23.x): curl redirect likely, untested', match: 'No', matchWhy: 'no match server for this version (Reboot covers S1-S19 only)', cosmetics: 7702 }),
  mk('25.11', 25, 11, 26787151, '5.3', { lobby: 'No', lobbyWhy: 'uses EOS Easy Anti-Cheat and EOS login (24.x+): this method does not apply', match: 'No', matchWhy: 'no match server for this version (Reboot covers S1-S19 only)', cosmetics: 0 }),
];
const cfg = { displayName: 'testingacc', mode: 'practice', bots: 20, botsAI: false, infiniteMaterials: true, infiniteAmmo: false, lateGame: false };
const logs = (n) => Array.from({ length: n }, (_, i) => {
  const lv = i % 37 === 0 ? 'bad' : i % 11 === 0 ? 'warn' : null;
  const t = lv === 'bad' ? `Error: the redirect did not confirm (see offline\\redirect-client.log). Stopped for safety. (#${i})`
    : lv === 'warn' ? `Warning: the match server did not get ready (see offline\\userdir-server\\Saved\\Logs). The lobby still works. (#${i})`
    : i % 5 === 0 ? `redirect https://fortnite-public-service-prod11.ol.epicgames.com/fortnite/api/game/v2/profile/4e8efc51eb94432fb4b584c6ae052f83/client/QueryProfile?profileId=athena&rvn=-1 -> http://127.0.0.1:3551/... (#${i})`
    : `Backend ready on 127.0.0.1:3551 (local account "testingacc"). (#${i})`;
  return { type: 'log', text: t, level: lv, ts: `22:${String(10 + (i / 60 | 0) % 50).padStart(2, '0')}:${String(i % 60).padStart(2, '0')}` };
});
const state = (extra, sel) => ({ type: 'state', builds: many, config: { ...cfg, ...extra }, selected: sel, root: 'S:\\FortniteBuilds' });

const scenarios = [
  { id: '20-match-server-bots-form', vp: [1200, 800], dpr: 1, msgs: [state({ mode: 'bots', bots: 35 }, '7.40')] },
  { id: '21-match-server-ai-on-900-125', vp: [900, 640], dpr: 1.25, msgs: [state({ mode: 'bots', botsAI: true }, '17.50')] },
  // the native selftest opens the menu without a pointer over its button: same here (no click, no hover)
  { id: '22-build-menu-toast', vp: [1200, 800], dpr: 1, msgs: [state({}, '12.41'), ...logs(5), { type: 'toast', text: 'Desktop shortcut created', level: 'ok' }], menu: 'buildMenu' },
  { id: '23-keyboard-focus-switch', vp: [1200, 800], dpr: 1, msgs: [state({ mode: 'bots' }, '17.50')], tabs: 18 },
];

const browser = await chromium.launch({ headless: true });
const report = [];
try {
  for (const s of scenarios) {
    const ctx = await browser.newContext({ viewport: { width: s.vp[0], height: s.vp[1] }, deviceScaleFactor: s.dpr, reducedMotion: 'reduce' });
    const page = await ctx.newPage();
    const errors = [];
    page.on('pageerror', (e) => errors.push(e.message));
    await page.goto('file://' + path.resolve(ui, 'index.html'));
    await page.evaluate(() => document.fonts.ready);
    await page.evaluate((msgs) => { msgs.forEach((m) => window.__fnoffline(m)); }, s.msgs);
    if (s.menu) await page.evaluate((id) => document.getElementById(id).classList.add('open'), s.menu);
    for (let i = 0; i < (s.tabs || 0); i++) await page.keyboard.press('Tab');
    await page.waitForTimeout(120);
    await page.screenshot({ path: path.join(out, s.id + '.png') });
    report.push({ id: s.id, viewport: `${s.vp[0]}x${s.vp[1]}@${s.dpr}x`, errors });
    await ctx.close();
  }
} finally { await browser.close(); }
for (const r of report) console.log(r.id, r.viewport, r.errors.length ? 'JS ERRORS: ' + r.errors.join(' | ') : 'ok');
