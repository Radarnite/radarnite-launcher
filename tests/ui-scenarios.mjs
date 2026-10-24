// Visual scenarios of the Radarnite Launcher page (ui/index.html) with mocked host messages.
// usage: node ui-scenarios.mjs <ui dir> <out dir>   (needs playwright; headless Chromium)
import { createRequire } from 'node:module';
import path from 'node:path';
import fs from 'node:fs';
const require = createRequire(process.env.PW_DIR || import.meta.url);
const { chromium } = require('playwright');
const [ui, out] = process.argv.slice(2);
fs.mkdirSync(out, { recursive: true });

const b1750 = { name: '17.50', dir: 'S:\\Fortnite\\FortniteBuilds\\17.50', version: '17.50', major: 17, minor: 50, cl: 17388565, engine: '4.26', lobby: 'Yes', lobbyWhy: 'UE4 (3.x-19.x): curl redirect + -noeac launch', match: 'Experimental', matchWhy: "Reboot: S16-S19 and S1-S2 'might not work perfectly'", reboot: true, cosmetics: 5338, firewall: 'OK' };
const mk = (v, maj, min, cl, eng, extra = {}) => ({ ...b1750, name: v, dir: `S:\\Fortnite\\FortniteBuilds\\${v}`, version: v, major: maj, minor: min, cl, engine: eng, ...extra });
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

const scenarios = [
  { id: '01-one-build-practice-100', vp: [1200, 800], dpr: 1, msgs: [{ type: 'state', builds: [b1750], config: cfg, selected: '17.50', root: 'S:\\Fortnite\\FortniteBuilds' }] },
  { id: '02-many-builds-bots-activity-125', vp: [960, 640], dpr: 1.25, msgs: [{ type: 'state', builds: many, config: { ...cfg, mode: 'bots', bots: 35 }, selected: '17.50', root: 'S:\\Fortnite\\FortniteBuilds' }, ...logs(400)], open: true },
  { id: '03-launching-150', vp: [800, 534], dpr: 1.5, msgs: [{ type: 'state', builds: many, config: { ...cfg, mode: 'bots' }, selected: '17.50', root: 'S:\\Fortnite\\FortniteBuilds' }, { type: 'session', session: { state: 'launching', version: '17.50', stages: { backend: 'ok', server: 'run', game: '' } } }] },
  { id: '04-notices-firewall-missing', vp: [1200, 800], dpr: 1, msgs: [{ type: 'state', builds: many, config: { ...cfg, mode: 'lobby', displayName: 'AVeryLongPlayerNameThatGoesOn' }, selected: '12.41', root: 'S:\\Fortnite\\FortniteBuilds' }] },
  { id: '05-unsupported', vp: [1200, 800], dpr: 1, msgs: [{ type: 'state', builds: many, config: cfg, selected: '25.11', root: 'S:\\Fortnite\\FortniteBuilds' }] },
  { id: '06-error-state', vp: [1200, 800], dpr: 1, msgs: [{ type: 'state', builds: [b1750], config: cfg, selected: '17.50', root: 'S:\\Fortnite\\FortniteBuilds' }, ...logs(30), { type: 'session', session: { state: 'error', version: '17.50', message: 'Not starting: The Epic Games Launcher is open: quit it completely (also from the tray) and try again.', stages: { backend: 'bad' } } }] },
  { id: '07-running', vp: [1200, 800], dpr: 1, msgs: [{ type: 'state', builds: [b1750], config: { ...cfg, mode: 'bots' }, selected: '17.50', root: 'S:\\Fortnite\\FortniteBuilds' }, { type: 'session', session: { state: 'running', version: '17.50', stages: { backend: 'ok', server: 'ok', game: 'ok' } } }] },
  { id: '08-empty', vp: [1200, 800], dpr: 1, msgs: [{ type: 'state', builds: [], config: cfg, selected: null, root: 'S:\\Fortnite\\FortniteBuilds' }] },
  { id: '09-minimum-760x520', vp: [760, 520], dpr: 1, msgs: [{ type: 'state', builds: many, config: { ...cfg, mode: 'bots' }, selected: '17.50', root: 'S:\\Fortnite\\FortniteBuilds' }] },
  { id: '10-loading', vp: [1200, 800], dpr: 1, msgs: [] },
  { id: '12-bots-no-ai-125', vp: [1180, 760], dpr: 1.25, msgs: [{ type: 'state', builds: [b1750], config: { ...cfg, mode: 'bots' }, selected: '17.50', root: 'S:\\Fortnite\\FortniteBuilds' }] },
  { id: '13-bots-ai-on-warning', vp: [1200, 800], dpr: 1, msgs: [{ type: 'state', builds: [b1750], config: { ...cfg, mode: 'bots', botsAI: true }, selected: '17.50', root: 'S:\\Fortnite\\FortniteBuilds' }] },
  { id: '14-match-waiting', vp: [1200, 800], dpr: 1, msgs: [{ type: 'state', builds: [b1750], config: { ...cfg, mode: 'bots' }, selected: '17.50', root: 'S:\\Fortnite\\FortniteBuilds' }, { type: 'session', session: { state: 'running', version: '17.50', stages: { backend: 'ok', server: 'ok', game: 'ok' }, match: { phase: 'waiting', detail: 'Waiting for you: press PLAY in the game' } } }] },
  { id: '15-match-warmup-150', vp: [880, 587], dpr: 1.5, msgs: [{ type: 'state', builds: [b1750], config: { ...cfg, mode: 'bots' }, selected: '17.50', root: 'S:\\Fortnite\\FortniteBuilds' }, { type: 'session', session: { state: 'running', version: '17.50', stages: { backend: 'ok', server: 'ok', game: 'ok' }, match: { phase: 'warmup', detail: 'Warm-up island · bus in 60 s' } } }] },
  { id: '16-match-storm', vp: [1200, 800], dpr: 1, msgs: [{ type: 'state', builds: [b1750], config: { ...cfg, mode: 'bots' }, selected: '17.50', root: 'S:\\Fortnite\\FortniteBuilds' }, { type: 'session', session: { state: 'running', version: '17.50', stages: { backend: 'ok', server: 'ok', game: 'ok' }, match: { phase: 'storm', detail: 'Storm' } } }] },
  { id: '17-match-server-down', vp: [1200, 800], dpr: 1, msgs: [{ type: 'state', builds: [b1750], config: { ...cfg, mode: 'bots' }, selected: '17.50', root: 'S:\\Fortnite\\FortniteBuilds' }, { type: 'session', session: { state: 'running', version: '17.50', stages: { backend: 'ok', server: 'ok', game: 'ok' }, match: { phase: 'down', detail: 'The match server stopped' } } }, ...logs(12)], open: true },
  { id: '18-real-root-3.6-14.30-17.50', vp: [1200, 800], dpr: 1.25, msgs: [{ type: 'state', builds: [
      mk('3.6-CL-4019403', 3, 6, 4019403, '4.20', { dir: 'S:\\Fortnite\\FortniteBuilds\\3.6-CL-4019403\\Fortnite', match: 'Yes', matchWhy: 'Reboot: S3-S15 supported', cosmetics: 0, firewall: 'firewall rules missing for 6 programs' }),
      mk('14.30', 14, 30, 15341163, '4.26', { match: 'Yes', matchWhy: 'Reboot: S3-S15 supported', cosmetics: 4100 }), b1750],
      config: cfg, selected: '3.6-CL-4019403', root: 'S:\\Fortnite\\FortniteBuilds' },
      { type: 'log', text: 'Not a build, ignored: S:\\Fortnite\\FortniteBuilds\\14.30-damaged (FortniteGame\\Binaries\\Win64\\FortniteClient-Win64-Shipping.exe is missing (incomplete extraction?)).', level: 'warn' }], open: true },
  { id: '19-state-without-config', vp: [1200, 800], dpr: 1, msgs: [{ type: 'state', builds: [b1750], config: {}, selected: '17.50', root: 'S:\\Fortnite\\FortniteBuilds' }] },
  { id: '11-large-1920x1080', vp: [1920, 1040], dpr: 1, msgs: [{ type: 'state', builds: many, config: { ...cfg, mode: 'lobby' }, selected: '7.40', root: 'S:\\Fortnite\\FortniteBuilds' }, ...logs(60)], open: true },
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
    await page.evaluate((msgs) => { window.__dbg = 1; msgs.forEach((m) => window.__fnoffline(m)); }, s.msgs);
    if (s.open) await page.click('#actBtn');
    await page.waitForTimeout(120);
    // layout checks: nothing overflows the viewport horizontally, no clipped text in key controls
    const checks = await page.evaluate(() => {
      const r = [];
      const vw = innerWidth;
      for (const el of document.querySelectorAll('.tb, .rail, .ws, .lbar, .content, .hero, .card, .play, .chip, .mode, .bi'))
        { const b = el.getBoundingClientRect(); if (b.width && b.right > vw + 0.5) r.push(`overflow-x ${el.className} right=${b.right.toFixed(0)} > ${vw}`); }
      for (const el of document.querySelectorAll('.play, .mode .t, .chip, .act, .btn, .bi .v'))
        if (el.offsetParent && el.scrollWidth > el.clientWidth + 1) r.push(`clipped ${el.className}: "${el.textContent.trim().slice(0, 30)}"`);
      const corner = document.elementFromPoint(2, innerHeight - 2); 
      const doc = document.scrollingElement; if (doc.scrollHeight > innerHeight + 1) r.push('page scrolls');
      return r;
    });
    await page.screenshot({ path: path.join(out, s.id + '.png') });
    report.push({ id: s.id, viewport: `${s.vp[0]}x${s.vp[1]}@${s.dpr}x`, issues: checks, errors });
    await ctx.close();
  }
} finally { await browser.close(); }
fs.writeFileSync(path.join(out, 'report.json'), JSON.stringify(report, null, 2));
for (const r of report) console.log(r.id, r.viewport, r.issues.length ? r.issues.join(' | ') : 'ok', r.errors.length ? 'JS ERRORS: ' + r.errors.join(' | ') : '');
