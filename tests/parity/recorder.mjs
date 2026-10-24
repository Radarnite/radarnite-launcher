// Parity recorder: records every HTTP exchange and WebSocket frame (XMPP, matchmaker) between the game (and the
// launcher and the match server) and the Node backend, with timing, as a session for compare.mjs to replay.
//
// It runs inside the backend's node process, in front of server.mjs: server.mjs still gets the public port (that port
// is baked into its answers: the hotfix ini's XMPP ServerPort, with its sha1/sha256/length, and the matchmaker's
// serviceUrl), but its HTTP server binds an ephemeral 127.0.0.1 port; the recorder listens on the public port and pipes
// every byte, unchanged, to it (game -> recorder port -> Node). A recorder bug can only lose records, never traffic.
//
// usage:
//   with the launcher: in the console that starts RadarniteLauncher.exe,
//     set RN_PARITY_RECORD=<corpus dir>      (the session goes to <corpus dir>\<build>-<date>-<time>)
//     set NODE_OPTIONS=--import=file:///<absolute path of this file, forward slashes>
//   standalone: node recorder.mjs [--out <corpus dir>] [--name <session>] <server.mjs> --build <dir> --config <file> --port <n> [--no-match]
// Optional: RN_PARITY_NAME (session name), RN_PARITY_MAX_BODY (response bodies above it are kept as length + sha256;
// default 1048576 bytes; request bodies are always kept whole).
import http from 'node:http';
import net from 'node:net';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { performance } from 'node:perf_hooks';
import { HttpStream, WsParser, JsonlWriter, messageJson, frameJson, copyTree } from './wire.mjs';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const SNAPSHOT_FILES = ['state.json', 'cosmetics.json', 'custom-cosmetics.json', 'keychain.json'];
const SNAPSHOT_DIRS = ['hotfix', 'cloud', 'ui'];

function stamp() {
  const d = new Date(), p = (n) => String(n).padStart(2, '0');
  return `${d.getFullYear()}${p(d.getMonth() + 1)}${p(d.getDate())}-${p(d.getHours())}${p(d.getMinutes())}${p(d.getSeconds())}`;
}

export function install(outDir, args, name) {
  const arg = (n) => { const i = args.indexOf(n); return i >= 0 ? args[i + 1] : undefined; };
  const build = arg('--build'), config = arg('--config');
  if (!build) return; // server.mjs prints its usage and exits
  const off = path.join(build, 'offline');
  const buildName = path.basename(path.resolve(build));
  let dir = path.join(outDir, name || `${buildName}-${stamp()}`.replace(/[^\w.-]+/g, '_'));
  for (let i = 2; fs.existsSync(dir); i++) dir = path.join(outDir, `${path.basename(dir).replace(/~\d+$/, '')}~${i}`);
  fs.mkdirSync(path.join(dir, 'inputs', 'offline'), { recursive: true });

  // inputs as server.mjs is about to read them
  const inputs = {};
  if (config && fs.existsSync(config)) { fs.copyFileSync(config, path.join(dir, 'inputs', 'config.json')); inputs['config.json'] = fs.statSync(config).size; }
  for (const f of SNAPSHOT_FILES) { const s = path.join(off, f); if (fs.existsSync(s)) { fs.copyFileSync(s, path.join(dir, 'inputs', 'offline', f)); inputs[`offline/${f}`] = fs.statSync(s).size; } }
  for (const d of SNAPSHOT_DIRS) { const s = path.join(off, d); if (fs.existsSync(s)) { copyTree(s, path.join(dir, 'inputs', 'offline', d)); inputs[`offline/${d}/`] = fs.readdirSync(s).length; } }

  const maxBody = Number(process.env.RN_PARITY_MAX_BODY || 1 << 20);
  const out = new JsonlWriter(path.join(dir, 'session.jsonl'));
  const T0 = performance.now();
  const now = () => Math.round((performance.now() - T0) * 10) / 10;
  let seq = 0, connSeq = 0;
  // only the flags: paths can hold the user name and are not needed to replay
  out.write({ type: 'meta', schema: 1, recorder: 'radarnite-parity-recorder/1', startedAt: new Date().toISOString(), build: buildName,
    flags: args.filter((a) => a === '--no-match'), port: Number(arg('--port')) || null, node: process.version, platform: process.platform, inputs });

  // final/: state.json, unknown-routes.log and cloud/ as they change (the launcher kills the backend: no exit hook runs)
  const seen = new Map();
  const syncOne = (src, dst) => {
    try { const st = fs.statSync(src); const k = `${st.size}:${st.mtimeMs}`; if (seen.get(src) === k) return; seen.set(src, k); fs.mkdirSync(path.dirname(dst), { recursive: true }); fs.copyFileSync(src, dst); } catch {}
  };
  let syncTimer = null;
  const syncFinal = () => {
    syncTimer = null;
    for (const f of ['state.json', 'unknown-routes.log']) syncOne(path.join(off, f), path.join(dir, 'final', f));
    try { for (const f of fs.readdirSync(path.join(off, 'cloud'))) syncOne(path.join(off, 'cloud', f), path.join(dir, 'final', 'cloud', f)); } catch {}
  };
  const scheduleSync = () => { if (!syncTimer) { syncTimer = setTimeout(syncFinal, 100); syncTimer.unref(); } };

  function onConnection(client, innerPort) {
    const conn = ++connSeq;
    const up = net.connect({ port: innerPort, host: '127.0.0.1', allowHalfOpen: true });
    client.setNoDelay(true); up.setNoDelay(true);
    let tapping = true, closing = false;
    const tap = (fn) => { if (!tapping) return; try { fn(); } catch (e) { tapping = false; out.write({ type: 'tap-error', conn, t: now(), message: String(e?.stack || e) }); } };
    const pending = []; // requests (and the upgrade) waiting for their answer, in order
    const answer = (ex, res) => {
      if (ex.upgrade) out.write({ type: 'ws-open', seq: ex.seq, conn, t: ex.t, req: ex.req, res: res ? messageJson(res, maxBody) : null, tRes: now() });
      else { out.write({ type: 'http', seq: ex.seq, conn, t: ex.t, tStart: ex.tStart, req: ex.req, res: res ? messageJson(res, maxBody) : null, tRes: res ? res.tEnd : now() }); scheduleSync(); }
    };
    let inBatch = [], outBatch = [];
    const inFrames = new WsParser((f) => inBatch.push(frameJson(f, maxBody)));
    const outFrames = new WsParser((f) => outBatch.push(frameJson(f, maxBody)));
    const reqs = new HttpStream('request', {
      now,
      onMessage: (m) => pending.push({ seq: ++seq, t: m.tEnd, tStart: m.tStart, upgrade: m.upgrade, req: messageJson(m) }),
      onRaw: (d) => { inBatch = []; inFrames.push(d); out.write({ type: 'ws-in', seq: ++seq, conn, t: now(), raw: d.toString('base64'), frames: inBatch }); },
    });
    const ress = new HttpStream('response', {
      now,
      isHead: () => pending[0]?.req.method === 'HEAD',
      onMessage: (m) => { const ex = pending.shift(); if (ex) answer(ex, m); },
      onRaw: (d) => { outBatch = []; outFrames.push(d); if (outBatch.length) out.write({ type: 'ws-out', seq: ++seq, conn, t: now(), frames: outBatch }); },
    });
    client.on('data', (d) => tap(() => reqs.push(d)));
    up.on('data', (d) => tap(() => ress.push(d)));
    client.pipe(up); up.pipe(client);
    client.on('end', () => tap(() => out.write({ type: 'end', seq: ++seq, conn, t: now(), by: 'client' })));
    up.on('end', () => tap(() => { ress.end(); out.write({ type: 'end', seq: ++seq, conn, t: now(), by: 'server' }); }));
    client.on('close', () => { if (!closing) tap(() => out.write({ type: 'close', seq: ++seq, conn, t: now(), by: 'client' })); closing = true; up.destroy(); });
    up.on('close', () => {
      tap(() => { for (const ex of pending.splice(0)) answer(ex, null); if (!closing) out.write({ type: 'close', seq: ++seq, conn, t: now(), by: 'server' }); });
      if (!closing) { closing = true; client.end(); setTimeout(() => client.destroy(), 2000).unref(); }
    });
    client.on('error', () => {}); up.on('error', () => {});
  }

  // server.mjs creates one HTTP server: bind it on an ephemeral port, put the recorder on the port it asked for
  const createServer = http.createServer;
  http.createServer = function (...a) {
    http.createServer = createServer;
    const srv = createServer.apply(this, a);
    const listen = srv.listen;
    srv.listen = function (port, host, cb) {
      if (typeof host === 'function') { cb = host; host = undefined; }
      const proxy = net.createServer({ allowHalfOpen: true }, (c) => onConnection(c, srv.address().port));
      proxy.on('error', (e) => srv.emit('error', e));
      listen.call(srv, 0, '127.0.0.1', () => proxy.listen(port, host || '127.0.0.1', () => {
        out.write({ type: 'listen', t: now(), port: proxy.address().port });
        process.stderr.write(`parity recorder: ${dir}\n`);
        cb?.();
      }));
      return srv;
    };
    return srv;
  };
  return dir;
}

const main = process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url);
if (main) {
  const a = process.argv.slice(2);
  let outDir = process.env.RN_PARITY_RECORD && process.env.RN_PARITY_RECORD !== '1' ? process.env.RN_PARITY_RECORD : path.join(HERE, 'corpus'), name = process.env.RN_PARITY_NAME;
  while (a[0]?.startsWith('--')) { const k = a.shift(); if (k === '--out') outDir = a.shift(); else if (k === '--name') name = a.shift(); else { console.error(`unknown option ${k}`); process.exit(2); } }
  const server = a.shift();
  if (!server) { console.error('usage: node recorder.mjs [--out <corpus dir>] [--name <session>] <server.mjs> --build <dir> --config <file> --port <n> [--no-match]'); process.exit(2); }
  process.argv = [process.argv[0], path.resolve(server), ...a];
  install(path.resolve(outDir), a, name);
  await import(pathToFileURL(path.resolve(server)).href);
} else if (process.env.RN_PARITY_RECORD && /server\.mjs$/i.test(process.argv[1] || '')) {
  const v = process.env.RN_PARITY_RECORD;
  install(path.resolve(v === '1' ? path.join(HERE, 'corpus') : v), process.argv.slice(2), process.env.RN_PARITY_NAME);
}
