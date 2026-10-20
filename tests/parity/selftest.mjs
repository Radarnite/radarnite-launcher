// Self-test of the parity harness (no Rust needed):
// 1. recorder round trip: Node vs Node behind recorder.mjs gives 0 differences (the recorder is transparent), and the
//    sessions it recorded replay Node vs Node with 0 differences and the same actions as the originals;
// 2. negative controls: copies of server.mjs with one behaviour changed each must give differences (so the
//    normalization does not hide what a port could get wrong).
// usage: node selftest.mjs [--server <server.mjs>] [--only round | --only <mutation name part>] [corpus dir]
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { spawn, spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { findSessions, readSession } from './wire.mjs';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const args = process.argv.slice(2);
const flag = (n, d) => { const i = args.indexOf(n); return i >= 0 ? args.splice(i, 2)[1] : d; };
const SERVER = path.resolve(flag('--server', path.join(HERE, '..', '..', 'backend', 'server.mjs')));
const ONLY = flag('--only', null);
const CORPUS = path.resolve(args[0] || path.join(HERE, 'synthetic'));
const SRC = fs.readFileSync(SERVER, 'utf8');

// name, sessions to replay, [find, replace] on server.mjs
const MUTATIONS = [
  ['routes match case-sensitively', ['routes'], [".replace(/\\*/g, '.*') + '$', 'i')", ".replace(/\\*/g, '.*') + '$')"]],
  ['timestamps without milliseconds', ['mcp-styles'], ['const now = () => new Date().toISOString();', "const now = () => new Date().toISOString().replace(/\\.\\d{3}Z$/, 'Z');"]],
  ['204 answered as 200', ['routes'], ['const E = (res) => { res.writeHead(204);', 'const E = (res) => { res.writeHead(200);']],
  ['JSON content type with charset', ['routes'], ["res.writeHead(code, { 'Content-Type': 'application/json',", "res.writeHead(code, { 'Content-Type': 'application/json; charset=utf-8',"]],
  ['BOM not stripped', ['inputs-bom'], ["return JSON.parse(fs.readFileSync(f, 'utf8').replace(/^\\uFEFF/, ''));", "return JSON.parse(fs.readFileSync(f, 'utf8'));"]],
  ['unknown keys of state.json dropped', ['inputs-bom'], ['fs.writeFileSync(statePath, JSON.stringify(state));', 'fs.writeFileSync(statePath, JSON.stringify({ ...state, _help: undefined, customKey: undefined }));']],
  ['server id from another md5', ['seasons-ua', 'routes'], ["update('server' + ACCOUNT_ID)", "update('Server' + ACCOUNT_ID)"]],
  ['token one byte shorter', ['lobby-3.6'], ["'eg1~offline-' + crypto.randomBytes(16)", "'eg1~offline-' + crypto.randomBytes(15)"]],
  ['client_id constant typo', ['lobby-3.6'], ["client_id: 'ec684b8c687f479fadea3cb2ad83f5c6'", "client_id: 'ec684b8c687f479fadea3cb2ad83f5c7'"]],
  ['XMPP port in the hotfix ini', ['lobby-3.6'], ["`ServerPort=${PORT}`", "`ServerPort=${PORT + 1}`"]],
  ['unknown-routes.log not deduplicated', ['paths-edge'], ['if (!unknown.has(key)) {', 'if (true) {']],
  ['matchmaker closes at 1500 ms', ['match'], ['ws.close(1000); }, 900);', 'ws.close(1000); }, 1500);']],
  ['Play sent at 700 ms', ['match'], ["name: 'Play' }); }, 400);", "name: 'Play' }); }, 700);"]],
  ['Queued state lower case', ['match'], ["state: 'Queued' }", "state: 'queued' }"]],
  ['estimatedWaitSec from 60 s', ['match'], ['90 - Math.round(', '60 - Math.round(']],
  ['--no-match ignored', ['no-match'], ["if (process.argv.includes('--no-match'))", 'if (false)']],
  ['XMPP close with code 1000', ['xmpp'], ['ws.send(\'<close xmlns="urn:ietf:params:xml:ns:xmpp-framing"/>\'); ws.close(); }', 'ws.send(\'<close xmlns="urn:ietf:params:xml:ns:xmpp-framing"/>\'); ws.close(1000); }']],
  ['XMPP presence attributes swapped', ['xmpp'], ['`<presence from="${jid}" to="${jid.split(\'/\')[0]}"`', '`<presence to="${jid.split(\'/\')[0]}" from="${jid}"`']],
  ['upgrade paths case-insensitive', ['xmpp'], ["if (!req.url.startsWith('/matchmaking')) return socket.destroy();", "if (!req.url.toLowerCase().startsWith('/matchmaking')) return socket.destroy();"]],
  ['cloud upload limit <= 4 MiB', ['paths-edge'], ['if (body.length < 4 * 1024 * 1024)', 'if (body.length <= 4 * 1024 * 1024)']],
  ['URL dot segments kept', ['paths-edge'], ["const url = new URL(req.url, 'http://127.0.0.1');", "const url = new URL(req.url.replace(/\\/\\.\\.?\\//g, '/_/'), 'http://127.0.0.1');"]],
];

const run = (argv) => new Promise((resolve) => {
  const child = spawn(process.execPath, [path.join(HERE, 'compare.mjs'), ...argv], { stdio: ['ignore', 'pipe', 'pipe'] });
  let out = '';
  child.stdout.on('data', (d) => (out += d)); child.stderr.on('data', (d) => (out += d));
  child.on('exit', (code) => resolve({ code, out }));
});
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'rn-parity-self-'));
let failures = 0;
const check = (ok, what, detail = '') => { console.log(`${ok ? 'ok  ' : 'FAIL'} ${what}`); if (!ok) { failures++; if (detail) console.log(detail.split('\n').map((l) => '     ' + l).join('\n')); } };

try {
  if (!ONLY || 'recorder round trip'.includes(ONLY)) {
    const rec = path.join(tmp, 'recorded');
    let r = await run(['--a', `node:${SERVER}`, '--b', `recorder:${SERVER}`, '--record-to', rec, '--no-cli', CORPUS]);
    check(r.code === 0, 'Node vs Node behind the recorder: 0 differences', r.out.slice(-3000));
    r = await run(['--a', `node:${SERVER}`, '--b', `node:${SERVER}`, '--no-cli', rec]);
    check(r.code === 0, 'the re-recorded sessions replay Node vs Node with 0 differences', r.out.slice(-3000));
    // what the client sent, in order: requests (tokens masked: the replay swaps them) and the bytes of each WebSocket
    // (TCP may split or join writes differently); not the comparator's health probe (its first connection) nor closes
    const sent = (s, skipConn) => {
      const out = [];
      for (const e of s.actions) {
        if (e.conn === skipConn || (e.type !== 'http' && e.type !== 'ws-open' && e.type !== 'ws-in')) continue;
        if (e.type === 'ws-in') { const last = out[out.length - 1]; if (last?.conn === e.conn && last.raw) { last.raw = Buffer.concat([last.raw, Buffer.from(e.raw, 'base64')]); continue; } out.push({ conn: e.conn, raw: Buffer.from(e.raw, 'base64') }); continue; }
        out.push({ text: `${e.type} ${e.req.method} ${e.req.url.replace(/eg1~offline-[0-9a-f]{32}/g, '<token>')}` });
      }
      return out.map((x) => x.text ?? `ws-in ${x.raw.toString('base64')}`).join('\n');
    };
    for (const d of findSessions([CORPUS])) {
      const a = readSession(d), b = fs.existsSync(path.join(rec, a.name)) ? readSession(path.join(rec, a.name)) : null;
      const same = b && sent(a) === sent(b, b.actions[0]?.conn);
      check(same, `re-recorded ${a.name}: the client sent the same requests and WebSocket bytes`);
    }
  }
  for (const [name, sessions, [find, replace]] of MUTATIONS) {
    if (ONLY && !name.includes(ONLY)) continue;
    const n = SRC.split(find).length - 1;
    if (n !== 1) { check(false, `mutation "${name}": anchor found ${n} times in server.mjs (expected 1)`); continue; }
    const dir = path.join(tmp, name.replace(/\W+/g, '-'));
    fs.mkdirSync(dir, { recursive: true });
    fs.writeFileSync(path.join(dir, 'server.mjs'), SRC.replace(find, () => replace)); // a function: no $' patterns
    const syntax = spawnSync(process.execPath, ['--check', path.join(dir, 'server.mjs')], { encoding: 'utf8' });
    if (syntax.status !== 0) { check(false, `mutation "${name}": the mutant does not parse`, syntax.stderr.slice(-800)); continue; }
    const r = await run(['--a', `node:${SERVER}`, '--b', `node:${path.join(dir, 'server.mjs')}`, '--no-cli', ...sessions.flatMap((s) => ['--only', s]), CORPUS]);
    const m = /parity: (\d+) difference/.exec(r.out);
    // caught for the right reason: the mutant ran (a backend that did not start is a difference too)
    const started = !/^ {4}startup$/m.test(r.out);
    check(r.code === 1 && m && Number(m[1]) > 0 && started, `mutation "${name}" is caught (${m ? m[1] : 0} difference(s) in ${sessions.join(', ')})`, r.code === 1 && started ? '' : r.out.slice(-1500));
  }
} finally { fs.rmSync(tmp, { recursive: true, force: true }); }
console.log(failures ? `selftest: ${failures} failure(s)` : 'selftest: ok');
process.exit(failures ? 1 : 0);
