// Parity comparator: replays recorded sessions against two backends, each with its own temp build dir and config.json,
// and diffs what they answer: HTTP status, framing, headers and bodies; WebSocket handshakes, frames (XMPP and
// matchmaker) and their timing; the files left in <build>/offline (state.json, unknown-routes.log, cloud/); and the
// command line (usage and port-in-use exit codes). Volatile values are normalized, everything else must match:
// - timestamps within a day of now -> <now>, but only when exactly YYYY-MM-DDTHH:MM:SS.sssZ (anything else is a diff);
// - access/refresh tokens and random hex ids -> aliases numbered per run, so equalities survive (the token issued is
//   the one verify echoes; one matchId across frames). Hex values the client sent, the constants in server.mjs and the
//   configured account id (and its md5 server id) are compared literally;
// - epoch milliseconds within a day of now -> <now-ms>; the matchmaker's estimatedWaitSec within 1; runs of identical
//   frames (the 1 s Queued poll) within 1; frame and close times within --time-tol ms;
// - the errorMessage of server.mjs's 500 (errors.com.epicgames.offline.internal) is the JS exception text (V8's
//   "URI malformed", "... is not iterable"): only that it is a string is compared.
// JSON key order is reported apart and only fails with --strict-order. Sessions replay in recorded order with the
// recorded pauses (capped by --max-gap); recorded tokens in requests are swapped for the ones each backend issued.
// Exit: 0 no differences, 1 differences, 2 harness error.
import fs from 'node:fs';
import os from 'node:os';
import net from 'node:net';
import path from 'node:path';
import crypto from 'node:crypto';
import { spawn } from 'node:child_process';
import { performance } from 'node:perf_hooks';
import { fileURLToPath } from 'node:url';
import { HttpConn, WsConn, requestBytes, readSession, findSessions, copyTree, listTree, decBody, sleep, routeTable, matchRoute, sha256 } from './wire.mjs';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const ROOT = path.resolve(HERE, '..', '..');
const RECORDER = path.join(HERE, 'recorder.mjs');
const USAGE = `usage: node compare.mjs [options] [corpus or session dir ...]   (default: synthetic/ and corpus/ next to this file)
  --a <target>        expected side (default node)
  --b <target>        side under test (default rust)
                      targets: node[:<server.mjs>]  rust[:<binary>]  recorder[:<server.mjs>] (node behind recorder.mjs)
  --server <file>     server.mjs for "node" (default backend/server.mjs)
  --rust <binary>     binary for "rust" (default $RN_RUST_BIN or backend-rs/out/radarnite-backend-linux-aarch64)
  --record-to <dir>   where the "recorder" target writes its sessions (default: a temp dir)
  --only <text>       only sessions whose name contains it (repeatable)
  --max-gap <ms>      cap on recorded pauses (default 1500)      --settle <ms>  wait after the last action (default 1500)
  --time-tol <ms>     WebSocket timing tolerance (default 150)   --strict-order JSON key order differences fail
  --max-print <n>     differences printed per session (default 40)
  --coverage          list the server.mjs routes no session hits
  --no-cli            skip the command-line checks              --json <file>  machine-readable report
  --keep              keep the temp build dirs`;

// ---------- options ----------
const opt = { a: 'node', b: 'rust', server: path.join(ROOT, 'backend', 'server.mjs'),
  rust: process.env.RN_RUST_BIN || path.join(ROOT, 'backend-rs', 'out', 'radarnite-backend-linux-aarch64'),
  recordTo: null, only: [], maxGap: 1500, settle: 1500, timeTol: 150, strictOrder: false, maxPrint: 40, coverage: false, cli: true, json: null, keep: false, dirs: [] };
{
  const a = process.argv.slice(2);
  const num = (k) => { const v = Number(a.shift()); if (!Number.isFinite(v)) die(`${k} needs a number`); return v; };
  while (a.length) {
    const k = a.shift();
    if (k === '--help' || k === '-h') { console.log(USAGE); process.exit(0); }
    else if (k === '--a') opt.a = a.shift();
    else if (k === '--b') opt.b = a.shift();
    else if (k === '--server') opt.server = path.resolve(a.shift());
    else if (k === '--rust') opt.rust = path.resolve(a.shift());
    else if (k === '--record-to') opt.recordTo = path.resolve(a.shift());
    else if (k === '--only') opt.only.push(a.shift());
    else if (k === '--max-gap') opt.maxGap = num(k);
    else if (k === '--settle') opt.settle = num(k);
    else if (k === '--time-tol') opt.timeTol = num(k);
    else if (k === '--max-print') opt.maxPrint = num(k);
    else if (k === '--strict-order') opt.strictOrder = true;
    else if (k === '--coverage') opt.coverage = true;
    else if (k === '--no-cli') opt.cli = false;
    else if (k === '--json') opt.json = path.resolve(a.shift());
    else if (k === '--keep') opt.keep = true;
    else if (k.startsWith('--')) die(`unknown option ${k}\n${USAGE}`);
    else opt.dirs.push(path.resolve(k));
  }
}
function die(msg) { console.error(msg); process.exit(2); }

// ---------- processes ----------
const children = new Set();
const killAll = () => { for (const c of children) try { c.kill('SIGKILL'); } catch {} };
process.on('exit', killAll);
for (const s of ['SIGINT', 'SIGTERM']) process.on(s, () => { killAll(); process.exit(130); });

function target(spec, letter) {
  const i = spec.indexOf(':'), kind = i < 0 ? spec : spec.slice(0, i), arg = i < 0 ? '' : spec.slice(i + 1);
  if (kind === 'node' || kind === 'recorder') {
    const src = path.resolve(arg || opt.server);
    if (!fs.existsSync(src)) die(`${letter}: ${src} not found`);
    const label = kind === 'node' ? `node ${path.relative(process.cwd(), src) || src}` : `recorder.mjs + node ${path.relative(process.cwd(), src) || src}`;
    return { letter, kind, label, src, argv: (name, args) => kind === 'node' ? [process.execPath, [src, ...args]]
      : [process.execPath, [RECORDER, '--out', opt.recordTo, '--name', name, src, ...args]] };
  }
  if (kind === 'rust') {
    const bin = path.resolve(arg || opt.rust);
    try { fs.accessSync(bin, fs.constants.X_OK); } catch { die(`${letter}: Rust backend not found or not executable: ${bin}`); }
    return { letter, kind, label: path.relative(process.cwd(), bin) || bin, argv: (name, args) => [bin, args] };
  }
  die(`unknown target "${spec}" (node, rust, recorder, optionally with :<path>)`);
}

function start(t, name, args, port) {
  const [cmd, argv] = t.argv(name, args);
  const child = spawn(cmd, argv, { stdio: ['ignore', 'pipe', 'pipe'] });
  children.add(child); child.on('exit', () => children.delete(child));
  const t0 = performance.now();
  return new Promise((resolve) => {
    let out = '', err = '', done = false;
    const fin = (r) => { if (done) return; done = true; clearTimeout(timer); resolve({ child, stderr: () => err, ...r }); };
    const timer = setTimeout(() => fin({ error: 'no "ready 127.0.0.1:<port>" line within 10 s' }), 10000);
    child.stdout.on('data', (d) => {
      out += d;
      const m = /^ready 127\.0\.0\.1:(\d+)\r?$/m.exec(out);
      if (m) fin(m[1] === String(port) ? { ready: true, readyMs: performance.now() - t0 } : { ready: true, readyMs: performance.now() - t0, badLine: m[0].trim() });
    });
    child.stderr.on('data', (d) => { err += d; });
    child.on('error', (e) => fin({ error: e.message }));
    child.on('exit', (code, sig) => fin({ error: `exited with ${code ?? sig} before ready` }));
  });
}
async function stop(child) {
  if (child.exitCode !== null || child.signalCode !== null) return;
  const exited = new Promise((r) => child.once('exit', r));
  child.kill('SIGTERM');
  if (await Promise.race([exited.then(() => true), sleep(3000).then(() => false)])) return;
  child.kill('SIGKILL'); await exited;
}
async function exitOf(t, args, ms = 8000) {
  const [cmd, argv] = t.argv('cli', args);
  const child = spawn(cmd, argv, { stdio: ['ignore', 'ignore', 'ignore'] });
  children.add(child);
  const r = await Promise.race([new Promise((res) => child.on('exit', (code, sig) => res(code ?? sig))), sleep(ms).then(() => 'still running')]);
  if (r === 'still running') await stop(child);
  children.delete(child);
  return r;
}
const freePort = () => new Promise((res) => { const s = net.createServer().listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });

// ---------- replay ----------
const TOKEN_RE = /eg1~offline-[0-9a-f]{32}(?![0-9a-f])|offline-refresh-[0-9a-f]{16}(?![0-9a-f])/g;
function buildRequest(req, port, tokens) {
  const sub = (x) => (tokens.size ? x.replace(TOKEN_RE, (t) => tokens.get(t) ?? t) : x);
  let body = decBody(req);
  if (tokens.size && body.length && !req.bodyEnc && !req.bodyFill) body = Buffer.from(sub(body.toString('utf8')), 'utf8');
  const framed = req.headers.some(([k]) => /^(content-length|transfer-encoding)$/i.test(k));
  const headers = [];
  for (const [k, v] of req.headers) {
    const n = k.toLowerCase();
    if (n === 'content-length' || n === 'transfer-encoding' || n === 'expect') continue;
    headers.push([k, n === 'host' ? `127.0.0.1:${port}` : sub(v)]);
  }
  if (framed || body.length) headers.push(['Content-Length', String(body.length)]);
  return requestBytes(req.method, sub(req.url), headers, body, req.version || 'HTTP/1.1');
}
// recorded token -> the one this backend issued at the same JSON path
function learnTokens(rec, got, tokens) {
  if (!rec?.body || rec.bodyEnc || rec.bodyOmitted || !/eg1~offline-|offline-refresh-/.test(rec.body)) return;
  let a, b; try { a = JSON.parse(rec.body); b = JSON.parse(got.body.toString('utf8')); } catch { return; }
  const walk = (x, y) => {
    if (typeof x === 'string') { if (typeof y === 'string' && x !== y && /^(eg1~offline-|offline-refresh-)/.test(x) && !tokens.has(x)) tokens.set(x, y); return; }
    if (x && y && typeof x === 'object' && typeof y === 'object') for (const k of Object.keys(x)) if (k in y) walk(x[k], y[k]);
  };
  walk(a, b);
}
async function replay(s, port) {
  const t0 = performance.now(), now = () => performance.now() - t0;
  const http = new Map(), ws = new Map(), conns = new Map(), tokens = new Map();
  let sched = 0, prev = null;
  for (const ev of s.actions) {
    if (prev != null) sched += Math.min(Math.max(0, ev.t - prev), opt.maxGap);
    prev = ev.t;
    if (sched - now() > 1) await sleep(sched - now());
    const c = conns.get(ev.conn);
    if (ev.type === 'http') {
      let hc = c;
      if (!(hc instanceof HttpConn)) conns.set(ev.conn, hc = new HttpConn(port, now));
      let m;
      try { m = await hc.request(buildRequest(ev.req, port, tokens), ev.req.method); } catch (e) { m = { error: e.code || e.message }; }
      http.set(ev.seq, m);
      if (!m.error) learnTokens(ev.res, m, tokens);
    } else if (ev.type === 'ws-open') {
      const wc = new WsConn(port, now, Infinity);
      conns.set(ev.conn, wc); ws.set(ev.conn, wc);
      await wc.open(buildRequest(ev.req, port, tokens), 3000);
    } else if (ev.type === 'ws-in') c?.send?.(Buffer.from(ev.raw, 'base64'));
    else if (ev.type === 'end') { if (c instanceof WsConn) c.end(); else c?.destroy(); }
    else if (ev.type === 'close') c?.destroy();
  }
  await sleep(opt.settle);
  for (const c of conns.values()) c.destroy();
  await sleep(50);
  return { http, ws };
}

// ---------- normalization ----------
const DAY = 864e5;
const ISO_EXACT = /^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z$/;
const TS_RE = /\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:[.,]\d+)?(?:Z|[+-]\d{2}:?\d{2})?/g;
class Norm {
  constructor({ whitelist, account, serverId, fixed }) { Object.assign(this, { whitelist, account, serverId, fixed }); this.aliases = new Map(); this.count = {}; }
  alias(kind, v) {
    const k = `${kind}:${v}`;
    if (!this.aliases.has(k)) { this.count[kind] = (this.count[kind] || 0) + 1; this.aliases.set(k, `<${kind}#${this.count[kind]}>`); }
    return this.aliases.get(k);
  }
  hex(h, kind) {
    if (!this.fixed && h === this.account) return '<account>';
    if (!this.fixed && h === this.serverId) return '<server-id>';
    return this.whitelist.has(h) ? h : this.alias(kind, h);
  }
  str(s) {
    return s
      .replace(/eg1~offline-[0-9a-f]{32}(?![0-9a-fA-F])/g, (t) => this.alias('token', t))
      .replace(/offline-refresh-[0-9a-f]{16}(?![0-9a-fA-F])/g, (t) => this.alias('refresh', t))
      .replace(TS_RE, (t) => { const ms = Date.parse(t); if (!Number.isFinite(ms) || Math.abs(ms - Date.now()) > DAY) return t; return ISO_EXACT.test(t) ? '<now>' : `<now, not YYYY-MM-DDTHH:MM:SS.sssZ: ${t}>`; })
      .replace(/(?<![0-9a-fA-F])[0-9a-f]{32}(?![0-9a-fA-F])/g, (h) => this.hex(h, 'hex32'))
      .replace(/(?<![0-9a-fA-F])[0-9a-f]{16}(?![0-9a-fA-F])/g, (h) => this.hex(h, 'hex16'));
  }
  // keys are walked sorted, so alias numbers do not depend on key order (reported apart)
  json(v, key) {
    if (typeof v === 'string') return this.str(v);
    if (typeof v === 'number') {
      if (key === 'estimatedWaitSec' && Number.isInteger(v) && v >= 5 && v <= 90) return '<wait>';
      if (Number.isInteger(v) && Math.abs(v - Date.now()) < DAY) return '<now-ms>';
      return v;
    }
    if (Array.isArray(v)) return v.map((x) => this.json(x));
    if (v && typeof v === 'object') { const o = {}; for (const k of Object.keys(v).sort()) o[this.str(k)] = this.json(v[k], k); return o; }
    return v;
  }
}
const utf8 = (b) => { const s = b.toString('utf8'); return Buffer.from(s, 'utf8').equals(b) ? s : null; };
const parseJson = (s) => { try { return { ok: true, v: JSON.parse(s.replace(/^﻿/, '')) }; } catch { return { ok: false }; } };

// ---------- differences ----------
const MISSING = Symbol('missing');
const keyPath = (k) => (/^[A-Za-z_$][\w$]*$/.test(k) ? `.${k}` : `[${JSON.stringify(k)}]`);
function jdiff(a, b, p, out) {
  if (out.length >= 50 || Object.is(a, b)) return;
  const ka = Array.isArray(a) ? 'array' : a === null ? 'null' : typeof a, kb = Array.isArray(b) ? 'array' : b === null ? 'null' : typeof b;
  if (ka !== kb) { out.push({ path: p, a, b }); return; }
  if (ka === 'array') {
    if (a.length !== b.length) out.push({ path: `${p}.length`, a: a.length, b: b.length });
    for (let i = 0; i < Math.min(a.length, b.length); i++) jdiff(a[i], b[i], `${p}[${i}]`, out);
    return;
  }
  if (ka === 'object') {
    for (const k of new Set([...Object.keys(a), ...Object.keys(b)])) {
      if (!(k in b)) out.push({ path: p + keyPath(k), a: a[k], b: MISSING });
      else if (!(k in a)) out.push({ path: p + keyPath(k), a: MISSING, b: b[k] });
      else jdiff(a[k], b[k], p + keyPath(k), out);
    }
    return;
  }
  out.push({ path: p, a, b });
}
function orderDiff(a, b, p = '$') {
  if (Array.isArray(a) && Array.isArray(b)) { for (let i = 0; i < Math.min(a.length, b.length); i++) { const r = orderDiff(a[i], b[i], `${p}[${i}]`); if (r) return r; } return null; }
  if (a && b && typeof a === 'object' && typeof b === 'object') {
    const ka = Object.keys(a).filter((k) => k in b), kb = Object.keys(b).filter((k) => k in a);
    if (ka.join('\0') !== kb.join('\0')) return { path: p, a: ka.join(','), b: kb.join(',') };
    for (const k of ka) { const r = orderDiff(a[k], b[k], p + keyPath(k)); if (r) return r; }
  }
  return null;
}
function textDiff(a, b) {
  let i = 0; while (i < a.length && i < b.length && a[i] === b[i]) i++;
  const from = Math.max(0, i - 40), cut = (s) => (from > 0 ? '…' : '') + s.slice(from, i + 80) + (s.length > i + 80 ? '…' : '');
  return [`at char ${i}: ${JSON.stringify(cut(a))}`, `at char ${i}: ${JSON.stringify(cut(b))}`];
}
class Diffs {
  constructor() { this.list = []; this.orders = []; }
  diff(where, what, a, b) { this.list.push({ where, what, a, b }); }
  order(where, what, o) { this.orders.push({ where, what, a: o.a, b: o.b, path: o.path }); }
}
const fmt = (v) => { if (v === MISSING) return '(missing)'; if (v === undefined) return '(none)'; const s = typeof v === 'string' ? JSON.stringify(v) : JSON.stringify(v) ?? String(v); return s.length > 220 ? s.slice(0, 220) + '…' : s; };

function cmpBody(where, label, a, b, nA, nB, D, fix = (v) => v) {
  if (a.equals(b)) return;
  const ta = utf8(a), tb = utf8(b);
  if (ta == null || tb == null) { D.diff(where, `${label} (binary)`, `${a.length} bytes, sha256 ${sha256(a).slice(0, 16)}`, `${b.length} bytes, sha256 ${sha256(b).slice(0, 16)}`); return; }
  const ja = parseJson(ta), jb = parseJson(tb);
  if (ja.ok && jb.ok && typeof ja.v === 'object' && typeof jb.v === 'object') {
    const out = []; jdiff(nA.json(fix(ja.v)), nB.json(fix(jb.v)), '$', out);
    for (const d of out) D.diff(where, `${label} ${d.path}`, d.a, d.b);
    if (!out.length) { const o = orderDiff(ja.v, jb.v); if (o) D.order(where, label, o); }
    return;
  }
  const sa = nA.str(ta), sb = nB.str(tb);
  if (sa !== sb) { const [x, y] = textDiff(sa, sb); D.diff(where, label, x, y); }
}
const IGNORED_HEADERS = new Set(['date', 'connection', 'keep-alive', 'content-length', 'transfer-encoding']);
function cmpHeaders(where, ha, hb, nA, nB, D) {
  const map = (h, n) => { const o = {}; for (const [k, v] of h || []) { const l = k.toLowerCase(); if (!IGNORED_HEADERS.has(l)) o[l] = o[l] == null ? n.str(v) : `${o[l]}, ${n.str(v)}`; } return o; };
  const a = map(ha, nA), b = map(hb, nB);
  for (const k of new Set([...Object.keys(a), ...Object.keys(b)])) if (a[k] !== b[k]) D.diff(where, `header ${k}`, a[k] ?? MISSING, b[k] ?? MISSING);
}
function cmpHttp(ev, ra, rb, nA, nB, D) {
  const where = `#${ev.seq} ${ev.req.method} ${ev.req.url.length > 140 ? ev.req.url.slice(0, 140) + '…' : ev.req.url}`;
  const sa = !ra ? 'not sent' : ra.error ? ra.error : null, sb = !rb ? 'not sent' : rb.error ? rb.error : null;
  if (sa || sb) { if (sa !== sb) D.diff(where, 'response', sa ?? `${ra.status}`, sb ?? `${rb.status}`); return; }
  if (ra.status !== rb.status) D.diff(where, 'status', ra.status, rb.status);
  if (ra.framing !== rb.framing) D.diff(where, 'framing (length = Content-Length, chunked, none, eof = until close)', ra.framing, rb.framing);
  cmpHeaders(where, ra.headers, rb.headers, nA, nB, D);
  cmpBody(where, 'body', ra.body, rb.body, nA, nB, D, internalError);
}
const internalError = (v) => (v && v.errorCode === 'errors.com.epicgames.offline.internal' && typeof v.errorMessage === 'string' ? { ...v, errorMessage: '<exception message>' } : v);

function frameNorm(f, n) {
  const o = { op: f.op, fin: f.fin, masked: f.masked };
  if (f.rsv) o.rsv = f.rsv;
  if (f.op === 8) { o.code = f.code ?? null; if (f.reason) o.reason = f.reason; return { o, wait: null }; }
  const b = decBody(f), t = utf8(b);
  let wait = null;
  if (t == null) o.data = 'base64:' + b.toString('base64');
  else { const j = parseJson(t); if (j.ok && j.v && typeof j.v === 'object') { o.data = n.json(j.v); wait = j.v.payload?.estimatedWaitSec ?? null; } else o.data = n.str(t); }
  return { o, wait };
}
function frameRuns(frames, n) {
  const out = [];
  for (const f of frames) {
    const { o, wait } = frameNorm(f, n), key = JSON.stringify(o), last = out[out.length - 1];
    if (last && last.key === key) { last.count++; continue; }
    out.push({ key, o, count: 1, t: f.t, wait });
  }
  return out;
}
const frameName = (o) => (o.op === 8 ? `close ${o.code ?? '(no code)'}` : typeof o.data === 'object' ? (o.data?.name ? `${o.data.name}${o.data.payload?.state ? ' ' + o.data.payload.state : ''}` : 'json') : String(o.data).slice(0, 40));
function cmpWs(ev, ca, cb, nA, nB, D) {
  const where = `ws#${ev.conn} ${ev.req.url}`;
  if (!ca || !cb) { D.diff(where, 'connection', ca ? 'opened' : 'not opened', cb ? 'opened' : 'not opened'); return; }
  const ha = ca.res, hb = cb.res;
  if (!ha || !hb) { if (!!ha !== !!hb) D.diff(where, 'handshake', ha ? `${ha.status}` : 'closed without an answer', hb ? `${hb.status}` : 'closed without an answer'); }
  else {
    if (ha.status !== hb.status) D.diff(where, 'handshake status', ha.status, hb.status);
    cmpHeaders(where, ha.headers, hb.headers, nA, nB, D);
    if (ha.status !== 101) cmpBody(where, 'handshake body', ha.body, hb.body, nA, nB, D);
  }
  const fa = frameRuns(ca.frames, nA), fb = frameRuns(cb.frames, nB);
  for (let i = 0; i < Math.max(fa.length, fb.length); i++) {
    const x = fa[i], y = fb[i], w = `${where} frame ${i + 1}`;
    if (!x || !y) { D.diff(w, 'frame', x ? `${frameName(x.o)} at ${x.t} ms` : MISSING, y ? `${frameName(y.o)} at ${y.t} ms` : MISSING); continue; }
    if (x.key !== y.key) {
      const out = []; jdiff(x.o, y.o, '$', out);
      for (const d of out) D.diff(w, `${frameName(x.o)} ${d.path}`, d.a, d.b);
      continue;
    }
    if (x.count !== y.count && !(Math.abs(x.count - y.count) <= 1 && Math.max(x.count, y.count) >= 2)) D.diff(w, `${frameName(x.o)} repeated`, `${x.count}x`, `${y.count}x`);
    if (x.wait != null && y.wait != null && Math.abs(x.wait - y.wait) > 1) D.diff(w, `${frameName(x.o)} estimatedWaitSec`, x.wait, y.wait);
    if (Math.abs(x.t - y.t) > opt.timeTol) D.diff(w, `${frameName(x.o)} time (ms after the upgrade request)`, x.t, y.t);
  }
  const ea = ca.serverEnd, eb = cb.serverEnd;
  if ((ea == null) !== (eb == null)) D.diff(where, 'server closed the connection', ea == null ? 'no' : `at ${ea} ms`, eb == null ? 'no' : `at ${eb} ms`);
  else if (ea != null && Math.abs(ea - eb) > opt.timeTol) D.diff(where, 'server close time (ms)', ea, eb);
}

function cmpFiles(dirA, dirB, nA, nB, D) {
  const fa = listTree(dirA), fb = listTree(dirB);
  delete fa['backend.log']; delete fb['backend.log'];
  for (const f of [...new Set([...Object.keys(fa), ...Object.keys(fb)])].sort()) {
    const where = `offline/${f}`;
    if (!fa[f] || !fb[f]) { D.diff(where, 'file', fa[f] ? 'present' : MISSING, fb[f] ? 'present' : MISSING); continue; }
    const a = fs.readFileSync(fa[f]), b = fs.readFileSync(fb[f]);
    if (f === 'unknown-routes.log') {
      const la = (utf8(a) ?? '').split('\n').map((l) => nA.str(l)), lb = (utf8(b) ?? '').split('\n').map((l) => nB.str(l));
      for (let i = 0; i < Math.max(la.length, lb.length); i++) if (la[i] !== lb[i]) D.diff(where, `line ${i + 1}`, la[i] ?? MISSING, lb[i] ?? MISSING);
    } else cmpBody(where, 'content', a, b, nA, nB, D);
  }
}

// ---------- one session ----------
const md5 = (s) => crypto.createHash('md5').update(s).digest('hex');
function hexIn(s, set) { for (const m of String(s).matchAll(/(?<![0-9a-fA-F])([0-9a-f]{32}|[0-9a-f]{16})(?![0-9a-fA-F])/g)) set.add(m[1]); }
async function runSide(t, s, port, base) {
  const build = path.join(base, s.meta.build || 'build');
  fs.mkdirSync(path.join(build, 'offline'), { recursive: true });
  copyTree(path.join(s.dir, 'inputs', 'offline'), path.join(build, 'offline'));
  const cfg = path.join(base, 'config.json');
  if (fs.existsSync(path.join(s.dir, 'inputs', 'config.json'))) fs.copyFileSync(path.join(s.dir, 'inputs', 'config.json'), cfg);
  const p = await start(t, s.name, ['--build', build, '--config', cfg, '--port', String(port), ...(s.meta.flags || [])], port);
  if (!p.ready) { await stop(p.child); return { error: p.error, stderr: p.stderr() }; }
  let account = null;
  try { const hc = new HttpConn(port, () => 0); const m = await hc.request(Buffer.from(`GET /offline/health HTTP/1.1\r\nHost: 127.0.0.1:${port}\r\n\r\n`), 'GET', 5000); hc.destroy(); account = JSON.parse(m.body.toString()).accountId ?? null; } catch {}
  const run = await replay(s, port);
  const crashed = p.child.exitCode !== null || p.child.signalCode !== null ? `exited with ${p.child.exitCode ?? p.child.signalCode} during the session` : null;
  await stop(p.child);
  return { readyMs: p.readyMs, badLine: p.badLine, account, run, crashed, offline: path.join(build, 'offline'), stderr: p.stderr() };
}

async function runSession(dir, A, B, tmp, constants) {
  const s = readSession(dir);
  const D = new Diffs();
  const port = await freePort();
  const base = path.join(tmp, s.name);
  const ra = await runSide(A, s, port, path.join(base, 'A'));
  if (ra.error) return { s, fatal: `${A.letter} (${A.label}) did not start: ${ra.error}\n${ra.stderr}` };
  const rb = await runSide(B, s, port, path.join(base, 'B'));
  if (rb.error) { D.diff('startup', 'backend', 'ready', `${rb.error}${rb.stderr ? ' — stderr: ' + rb.stderr.trim().slice(-400) : ''}`); return { s, D, ra, rb }; }
  if (ra.badLine || rb.badLine) D.diff('startup', 'ready line', ra.badLine || `ready 127.0.0.1:${port}`, rb.badLine || `ready 127.0.0.1:${port}`);
  if (ra.crashed || rb.crashed) D.diff('process', 'state', ra.crashed || 'running', rb.crashed || 'running');

  // literal hex: what the client sent, server.mjs constants, the configured account (and its md5 server id)
  const whitelist = new Set(constants);
  for (const ev of s.actions) { if (ev.req) { hexIn(ev.req.url, whitelist); for (const [, v] of ev.req.headers) hexIn(v, whitelist); if (ev.req.body && !ev.req.bodyEnc) hexIn(ev.req.body, whitelist); } for (const f of ev.frames || []) if (f.body && !f.bodyEnc) hexIn(f.body, whitelist); }
  let fixed = null;
  try { const c = JSON.parse(fs.readFileSync(path.join(s.dir, 'inputs', 'config.json'), 'utf8').replace(/^﻿/, '')); if (/^[0-9a-f]{32}$/.test(c.accountId || '')) fixed = c.accountId; } catch {}
  if (fixed) { whitelist.add(fixed); whitelist.add(md5('server' + fixed)); }
  const nA = new Norm({ whitelist, fixed: !!fixed, account: ra.account, serverId: ra.account && md5('server' + ra.account) });
  const nB = new Norm({ whitelist, fixed: !!fixed, account: rb.account, serverId: rb.account && md5('server' + rb.account) });

  // in session order, so aliases number alike on both sides
  for (const ev of s.actions) {
    if (ev.type === 'http') cmpHttp(ev, ra.run.http.get(ev.seq), rb.run.http.get(ev.seq), nA, nB, D);
    else if (ev.type === 'ws-open') cmpWs(ev, ra.run.ws.get(ev.conn), rb.run.ws.get(ev.conn), nA, nB, D);
  }
  cmpFiles(ra.offline, rb.offline, nA, nB, D);
  return { s, D, ra, rb };
}

async function cliChecks(A, B, tmp) {
  const D = new Diffs();
  const ea = await exitOf(A, []), eb = await exitOf(B, []);
  if (ea !== eb) D.diff('cli: no arguments', 'exit code', ea, eb);
  const dir = path.join(tmp, 'cli', 'busy'); fs.mkdirSync(path.join(dir, 'offline'), { recursive: true });
  const cfg = path.join(tmp, 'cli', 'config.json'); fs.writeFileSync(cfg, '{}');
  const blocker = net.createServer().listen(0, '127.0.0.1');
  await new Promise((r) => blocker.once('listening', r));
  const args = ['--build', dir, '--config', cfg, '--port', String(blocker.address().port)];
  const pa = await exitOf(A, args), pb = await exitOf(B, args);
  blocker.close();
  if (pa !== pb) D.diff('cli: port already in use', 'exit code', pa, pb);
  return D;
}

// ---------- main ----------
const A = target(opt.a, 'A'), B = target(opt.b, 'B');
const src = fs.readFileSync(A.src || opt.server, 'utf8');
const routes = routeTable(src);
const constants = new Set(src.match(/(?<![0-9a-fA-F])[0-9a-f]{32}(?![0-9a-fA-F])/g) || []);
const sessions = findSessions(opt.dirs.length ? opt.dirs : [path.join(HERE, 'synthetic'), path.join(HERE, 'corpus')])
  .filter((d) => !opt.only.length || opt.only.some((o) => path.basename(d).includes(o)));
if (!sessions.length) die('no sessions found (a session is a dir with session.jsonl)');
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'rn-parity-'));
if ((A.kind === 'recorder' || B.kind === 'recorder') && !opt.recordTo) opt.recordTo = path.join(tmp, 'recorded');
if (opt.recordTo) fs.mkdirSync(opt.recordTo, { recursive: true });

console.log(`parity: A = ${A.label}\n        B = ${B.label}\n        ${sessions.length} session(s), max gap ${opt.maxGap} ms, time tolerance ${opt.timeTol} ms`);
const report = { a: A.label, b: B.label, sessions: [] };
let total = 0, orders = 0, failed = 0, fatal = false;
const hit = new Set();
const printDiffs = (D, indent = '    ') => {
  let shown = 0, lastWhere = null;
  for (const d of D.list) {
    if (shown++ >= opt.maxPrint) { console.log(`${indent}… ${D.list.length - opt.maxPrint} more`); break; }
    if (d.where !== lastWhere) { console.log(`${indent}${d.where}`); lastWhere = d.where; }
    console.log(`${indent}  ${d.what}\n${indent}    A: ${fmt(d.a)}\n${indent}    B: ${fmt(d.b)}`);
  }
  for (const o of D.orders.slice(0, opt.strictOrder ? opt.maxPrint : 3)) console.log(`${indent}${opt.strictOrder ? '' : '(note) '}key order at ${o.where} ${o.what} ${o.path}\n${indent}    A: ${o.a}\n${indent}    B: ${o.b}`);
};

if (opt.cli) {
  const D = await cliChecks(A, B, tmp);
  total += D.list.length;
  console.log(`  ${'command line'.padEnd(34, ' ')} ${D.list.length ? `${D.list.length} diff(s)` : 'ok'}`);
  printDiffs(D);
  report.cli = D.list;
}
for (const dir of sessions) {
  const r = await runSession(dir, A, B, tmp, constants);
  const label = path.relative(HERE, dir).startsWith('..') ? dir : path.relative(HERE, dir);
  if (r.fatal) { fatal = true; console.log(`  ${label}: HARNESS ERROR ${r.fatal}`); report.sessions.push({ name: label, fatal: r.fatal }); continue; }
  for (const ev of r.s.actions) if (ev.type === 'http') { const m = matchRoute(routes, ev.req.method, ev.req.url); if (m) hit.add(m); }
  const n = r.D.list.length, no = opt.strictOrder ? r.D.orders.length : 0;
  total += n + no; orders += r.D.orders.length; if (n + no) failed++;
  const counts = `${r.s.actions.filter((e) => e.type === 'http').length} http, ${r.s.actions.filter((e) => e.type === 'ws-open').length} ws`;
  const ready = r.rb?.readyMs != null ? `; ready A ${Math.round(r.ra.readyMs)} ms, B ${Math.round(r.rb.readyMs)} ms` : '';
  console.log(`  ${label.padEnd(34, ' ')} ${n + no ? `${n + no} diff(s)` : 'ok'} (${counts}${ready}${r.D.orders.length && !opt.strictOrder ? `; ${r.D.orders.length} key-order note(s)` : ''})`);
  printDiffs(r.D);
  if ((n || r.rb?.crashed) && r.rb?.stderr?.trim()) console.log(`    B stderr: ${r.rb.stderr.trim().slice(-600)}`);
  report.sessions.push({ name: label, diffs: r.D.list.map((d) => ({ ...d, a: fmt(d.a), b: fmt(d.b) })), keyOrder: r.D.orders, readyMs: { a: r.ra.readyMs, b: r.rb?.readyMs } });
}
console.log(`routes hit: ${hit.size}/${routes.length} of server.mjs`);
if (opt.coverage) for (const r of routes) if (!hit.has(r)) console.log(`  not hit: ${r.method} ${r.pattern}`);
console.log(fatal ? 'parity: HARNESS ERROR' : total ? `parity: ${total} difference(s) in ${failed} session(s)${opt.cli && report.cli?.length ? ' + command line' : ''}` : `parity: 0 differences${orders ? ` (${orders} key-order note(s); --strict-order to fail on them)` : ''}`);
if (opt.json) fs.writeFileSync(opt.json, JSON.stringify({ ...report, total, fatal }, null, 1));
if (!opt.keep) fs.rmSync(tmp, { recursive: true, force: true }); else console.log(`kept ${tmp}`);
process.exit(fatal ? 2 : total ? 1 : 0);
