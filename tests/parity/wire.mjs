// Wire helpers shared by the parity recorder, the synthetic corpus generator and the comparator: HTTP/1.1 message
// parsing on raw byte streams, WebSocket (RFC 6455) frames, raw HTTP/WebSocket clients, session files (JSONL) and the
// route table of backend/server.mjs. Node built-ins only.
import fs from 'node:fs';
import net from 'node:net';
import path from 'node:path';
import crypto from 'node:crypto';

const CRLF = Buffer.from('\r\n');
const CRLF2 = Buffer.from('\r\n\r\n');
const EMPTY = Buffer.alloc(0);

export const sleep = (ms) => new Promise((r) => setTimeout(r, Math.max(0, ms)));
export const sha256 = (b) => crypto.createHash('sha256').update(b).digest('hex');
export const hget = (headers, name) => { const n = name.toLowerCase(); const h = (headers || []).find(([k]) => k.toLowerCase() === n); return h ? h[1] : undefined; };

// ---------- bodies in session files: UTF-8 text when it round-trips, base64 otherwise ----------
export function encBody(buf, max = Infinity) {
  if (!buf || buf.length === 0) return {};
  if (buf.length > max) return { bodyOmitted: true, bodyLength: buf.length, bodySha256: sha256(buf) };
  const s = buf.toString('utf8');
  return Buffer.from(s, 'utf8').equals(buf) ? { body: s } : { body: buf.toString('base64'), bodyEnc: 'base64' };
}
export function decBody(o) {
  if (!o) return EMPTY;
  if (o.bodyFill) return Buffer.alloc(o.bodyFill.length, o.bodyFill.char || 'A');
  if (o.body == null) return EMPTY;
  return o.bodyEnc === 'base64' ? Buffer.from(o.body, 'base64') : Buffer.from(o.body, 'utf8');
}

// ---------- HTTP/1.1 message stream (one direction of one connection) ----------
// kind 'request' | 'response'. Handlers: onMessage(msg), onRaw(bytes) for everything after an upgrade (101 on the
// response side, an Upgrade request on the request side), isHead() so a response to HEAD carries no body, now() for
// timing. msg = { method, url, version } or { version, status, reason }, plus headers [[name, value]], body (Buffer),
// framing ('length' | 'chunked' | 'eof' | 'none'), interim (1xx statuses seen before it), tStart, tEnd.
export class HttpStream {
  constructor(kind, h = {}) { this.kind = kind; this.h = h; this.buf = EMPTY; this.state = 'head'; this.msg = null; this.parts = []; this.interim = []; this.t0 = null; }
  push(chunk) {
    if (this.state === 'raw') return this.h.onRaw?.(chunk);
    if (this.state === 'dead') return;
    if (this.t0 == null) this.t0 = this.h.now?.() ?? 0;
    this.buf = this.buf.length ? Buffer.concat([this.buf, chunk]) : chunk;
    this.drain();
  }
  end() {
    if (this.state === 'eof' && this.msg) { this.parts.push(this.buf); this.buf = EMPTY; this.done(); }
    this.state = 'dead';
  }
  done() {
    const m = this.msg; m.body = this.parts.length ? Buffer.concat(this.parts) : EMPTY; m.tStart = this.t0; m.tEnd = this.h.now?.() ?? 0;
    if (this.interim.length) m.interim = this.interim;
    this.msg = null; this.parts = []; this.interim = []; this.t0 = this.buf.length ? m.tEnd : null;
    const upgrade = m.upgrade;
    if (upgrade) this.state = 'raw'; else if (this.state !== 'dead') this.state = 'head';
    this.h.onMessage?.(m);
    if (upgrade && this.buf.length) { const rest = this.buf; this.buf = EMPTY; this.h.onRaw?.(rest); }
  }
  drain() {
    for (;;) {
      if (this.state === 'head') {
        while (this.buf.length >= 2 && this.buf[0] === 13 && this.buf[1] === 10) this.buf = this.buf.subarray(2); // stray CRLF between messages
        const i = this.buf.indexOf(CRLF2);
        if (i < 0) { if (this.buf.length > (1 << 20)) this.state = 'dead'; return; }
        const lines = this.buf.subarray(0, i).toString('latin1').split('\r\n');
        this.buf = this.buf.subarray(i + 4);
        const first = lines.shift();
        const headers = lines.filter(Boolean).map((l) => { const c = l.indexOf(':'); return c < 0 ? [l, ''] : [l.slice(0, c), l.slice(c + 1).trim()]; });
        const te = /chunked/i.test(hget(headers, 'transfer-encoding') || ''), cl = hget(headers, 'content-length');
        if (this.kind === 'request') {
          const sp1 = first.indexOf(' '), sp2 = first.lastIndexOf(' ');
          this.msg = { method: first.slice(0, sp1), url: first.slice(sp1 + 1, sp2), version: first.slice(sp2 + 1), headers };
          this.msg.upgrade = !!hget(headers, 'upgrade') && /(^|,)\s*upgrade\s*(,|$)/i.test(hget(headers, 'connection') || '');
        } else {
          const [version, status, ...reason] = first.split(' ');
          this.msg = { version, status: Number(status), reason: reason.join(' '), headers };
          if (this.msg.status >= 100 && this.msg.status < 200 && this.msg.status !== 101) { this.interim.push(this.msg.status); this.msg = null; continue; }
          if (this.msg.status === 101) this.msg.upgrade = true;
        }
        const noBody = this.kind === 'response' && (this.msg.upgrade || this.h.isHead?.() || this.msg.status === 204 || this.msg.status === 304);
        if (noBody) { this.msg.framing = 'none'; this.done(); if (this.state === 'raw') return; continue; }
        if (te) { this.msg.framing = 'chunked'; this.state = 'chunked'; continue; }
        if (cl != null) { this.msg.framing = 'length'; this.need = Number(cl) || 0; this.state = 'length'; continue; }
        if (this.kind === 'request') { this.msg.framing = 'none'; this.done(); if (this.state === 'raw') return; continue; }
        this.msg.framing = 'eof'; this.state = 'eof'; return;
      }
      if (this.state === 'length') {
        if (this.buf.length < this.need) return;
        this.parts.push(this.buf.subarray(0, this.need)); this.buf = this.buf.subarray(this.need);
        this.done(); if (this.state === 'raw') return; continue;
      }
      if (this.state === 'chunked') {
        const i = this.buf.indexOf(CRLF);
        if (i < 0) return;
        const size = parseInt(this.buf.subarray(0, i).toString('latin1').split(';')[0].trim(), 16);
        if (Number.isNaN(size)) { this.state = 'dead'; return; }
        if (size === 0) {
          let end;
          if (this.buf.length >= i + 4 && this.buf[i + 2] === 13 && this.buf[i + 3] === 10) end = i + 4;
          else { const j = this.buf.indexOf(CRLF2, i); if (j < 0) return; end = j + 4; }
          this.buf = this.buf.subarray(end); this.done(); continue;
        }
        if (this.buf.length < i + 2 + size + 2) return;
        this.parts.push(this.buf.subarray(i + 2, i + 2 + size)); this.buf = this.buf.subarray(i + 4 + size);
        continue;
      }
      return; // 'eof' (until the connection ends), 'raw', 'dead'
    }
  }
}
export const messageJson = (m, maxBody) => m.method != null
  ? { method: m.method, url: m.url, version: m.version, headers: m.headers, ...encBody(m.body, maxBody) }
  : { version: m.version, status: m.status, reason: m.reason, headers: m.headers, framing: m.framing, ...(m.interim ? { interim: m.interim } : {}), ...encBody(m.body, maxBody) };

// ---------- WebSocket frames ----------
export class WsParser {
  constructor(onFrame) { this.buf = EMPTY; this.onFrame = onFrame; }
  push(d) {
    this.buf = this.buf.length ? Buffer.concat([this.buf, d]) : d;
    while (this.buf.length >= 2) {
      const b0 = this.buf[0], b1 = this.buf[1];
      let len = b1 & 127, off = 2;
      if (len === 126) { if (this.buf.length < 4) return; len = this.buf.readUInt16BE(2); off = 4; }
      else if (len === 127) { if (this.buf.length < 10) return; len = Number(this.buf.readBigUInt64BE(2)); off = 10; }
      const masked = !!(b1 & 128);
      if (this.buf.length < off + (masked ? 4 : 0) + len) return;
      const key = masked ? this.buf.subarray(off, off + 4) : null; if (masked) off += 4;
      const payload = Buffer.from(this.buf.subarray(off, off + len));
      if (key) for (let i = 0; i < payload.length; i++) payload[i] ^= key[i & 3];
      this.buf = this.buf.subarray(off + len);
      this.onFrame({ fin: !!(b0 & 128), rsv: (b0 >> 4) & 7, op: b0 & 15, masked, payload });
    }
  }
}
export function frameJson(f, maxBody) {
  const o = { fin: f.fin, op: f.op, masked: f.masked };
  if (f.rsv) o.rsv = f.rsv;
  if (f.op === 8) { o.code = f.payload.length >= 2 ? f.payload.readUInt16BE(0) : null; if (f.payload.length > 2) o.reason = f.payload.subarray(2).toString('utf8'); return o; }
  return { ...o, ...encBody(f.payload, maxBody) };
}
export function wsFrame(op, payload = EMPTY, { fin = true, mask = true, key = Buffer.from([0x37, 0xfa, 0x21, 0x3d]) } = {}) {
  const p = Buffer.from(Buffer.isBuffer(payload) ? payload : Buffer.from(String(payload)));
  const n = p.length, hl = n < 126 ? 2 : n < 65536 ? 4 : 10;
  const h = Buffer.alloc(hl + (mask ? 4 : 0));
  h[0] = (fin ? 0x80 : 0) | op;
  if (n < 126) h[1] = n; else if (n < 65536) { h[1] = 126; h.writeUInt16BE(n, 2); } else { h[1] = 127; h.writeBigUInt64BE(BigInt(n), 2); }
  if (mask) { h[1] |= 0x80; key.copy(h, hl); for (let i = 0; i < n; i++) p[i] ^= key[i & 3]; }
  return Buffer.concat([h, p]);
}

// ---------- raw clients (exact bytes on the wire; no URL normalization, no header rewriting) ----------
export function requestBytes(method, url, headers, body = EMPTY) {
  const head = `${method} ${url} HTTP/1.1\r\n` + headers.map(([k, v]) => `${k}: ${v}\r\n`).join('') + '\r\n';
  return Buffer.concat([Buffer.from(head, 'latin1'), body]);
}
function connect(port) {
  return new Promise((resolve, reject) => {
    const s = net.connect({ port, host: '127.0.0.1' });
    s.setNoDelay(true);
    s.once('connect', () => { s.off('error', reject); resolve(s); });
    s.once('error', reject);
  });
}

// One keep-alive HTTP connection; requests are sent one at a time.
export class HttpConn {
  constructor(port, now) { this.port = port; this.now = now; this.sock = null; this.waiter = null; this.method = 'GET'; }
  async open() {
    const s = await connect(this.port);
    this.parser = new HttpStream('response', { now: this.now, isHead: () => this.method === 'HEAD', onMessage: (m) => this.waiter?.(m) });
    s.on('data', (d) => this.parser.push(d));
    s.on('end', () => this.parser.end());
    s.on('close', () => { if (this.sock === s) this.sock = null; this.waiter?.(null); });
    s.on('error', () => {});
    this.sock = s;
  }
  async request(bytes, method, timeout = 15000, retry = true) {
    if (!this.sock) await this.open();
    this.method = method;
    const s = this.sock;
    const m = await new Promise((resolve) => {
      const timer = setTimeout(() => { this.waiter = null; resolve({ error: 'timeout' }); }, timeout);
      this.waiter = (x) => { clearTimeout(timer); this.waiter = null; resolve(x); };
      s.write(bytes);
    });
    if (m === null) { // closed without an answer: once more on a fresh connection (a keep-alive socket the server had dropped)
      this.destroy();
      return retry ? this.request(bytes, method, timeout, false) : { error: 'closed without a response' };
    }
    if (m.error) this.destroy();
    else if (/close/i.test(hget(m.headers, 'connection') || '') || m.version === 'HTTP/1.0') this.destroy();
    return m;
  }
  destroy() { this.sock?.destroy(); this.sock = null; }
}

// One WebSocket connection: open() sends the upgrade request as given and resolves with the handshake response (null if
// the server closed the socket without one); afterwards every frame the server sends is collected with its time.
export class WsConn {
  constructor(port, now, maxBody) { this.port = port; this.now = now; this.maxBody = maxBody; this.frames = []; this.res = null; this.serverEnd = null; this.closed = null; }
  async open(bytes, timeout = 5000) {
    this.t0 = this.now();
    const rel = () => Math.round((this.now() - this.t0) * 10) / 10;
    try { this.sock = await connect(this.port); } catch (e) { this.res = null; this.error = e.code || e.message; return null; }
    const ws = new WsParser((f) => this.frames.push({ t: rel(), ...frameJson(f, this.maxBody) }));
    return await new Promise((resolve) => {
      const timer = setTimeout(() => resolve(this.res), timeout);
      const http = new HttpStream('response', { now: this.now, onMessage: (m) => { this.res = m; clearTimeout(timer); resolve(m); }, onRaw: (d) => ws.push(d) });
      this.sock.on('data', (d) => http.push(d));
      this.sock.on('end', () => { this.serverEnd = rel(); http.end(); });
      this.sock.on('close', () => { this.closed = rel(); clearTimeout(timer); resolve(this.res); });
      this.sock.on('error', () => {});
      this.sock.write(bytes);
    });
  }
  send(bytes) { if (this.sock && !this.sock.destroyed) this.sock.write(bytes); }
  end() { if (this.sock && !this.sock.destroyed) this.sock.end(); }
  destroy() { this.sock?.destroy(); }
}

// ---------- session files ----------
// <session>/session.jsonl: { type: 'meta' } first, then events: 'listen', 'http' (req + res), 'ws-open' (upgrade req + res
// or null), 'ws-in' (raw bytes the client sent after the upgrade, base64, with the frames they complete), 'ws-out'
// (frames the server sent), 'end' / 'close' (by 'client' or 'server'). Every event has seq (order in which the recorder
// saw the client act) and t (ms since the recording started). <session>/inputs/ = config.json + offline/ inputs as they
// were at start; <session>/final/ = state.json, unknown-routes.log, cloud/ as last seen.
export class JsonlWriter {
  constructor(file) { this.fd = fs.openSync(file, 'a'); }
  write(o) { fs.writeSync(this.fd, JSON.stringify(o) + '\n'); }
  close() { try { fs.closeSync(this.fd); } catch {} }
}
const ACTIONS = new Set(['http', 'ws-open', 'ws-in']);
export function readSession(dir) {
  const file = path.join(dir, 'session.jsonl');
  const lines = [];
  for (const l of fs.readFileSync(file, 'utf8').split('\n')) { if (!l.trim()) continue; try { lines.push(JSON.parse(l)); } catch { /* a line cut by a killed process */ } }
  const meta = lines.find((l) => l.type === 'meta') || {};
  const events = lines.filter((l) => l.type !== 'meta' && l.seq != null).sort((a, b) => a.seq - b.seq);
  const actions = events.filter((e) => ACTIONS.has(e.type) || ((e.type === 'end' || e.type === 'close') && e.by === 'client'));
  return { dir, name: path.basename(dir), meta, events, actions };
}
// session dirs under each argument (a session dir itself, or a dir of them, one level deep)
export function findSessions(dirs) {
  const out = [];
  for (const d of dirs) {
    if (!fs.existsSync(d)) continue;
    if (fs.existsSync(path.join(d, 'session.jsonl'))) { out.push(path.resolve(d)); continue; }
    for (const e of fs.readdirSync(d, { withFileTypes: true }).sort((a, b) => (a.name < b.name ? -1 : 1)))
      if (e.isDirectory() && fs.existsSync(path.join(d, e.name, 'session.jsonl'))) out.push(path.resolve(d, e.name));
  }
  return out;
}
export function copyTree(src, dst) {
  if (!fs.existsSync(src)) return;
  fs.mkdirSync(dst, { recursive: true });
  for (const e of fs.readdirSync(src, { withFileTypes: true })) {
    const s = path.join(src, e.name), d = path.join(dst, e.name);
    if (e.isDirectory()) copyTree(s, d); else if (e.isFile()) fs.copyFileSync(s, d);
  }
}
export function listTree(dir, base = dir, out = {}) {
  if (!fs.existsSync(dir)) return out;
  for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
    const p = path.join(dir, e.name);
    if (e.isDirectory()) listTree(p, base, out); else if (e.isFile()) out[path.relative(base, p).split(path.sep).join('/')] = p;
  }
  return out;
}

// ---------- route table of server.mjs (same pattern -> RegExp rule as its on()) ----------
export function routeTable(src) {
  const out = [];
  for (const m of src.matchAll(/^on\('([A-Z]+)', '([^']+)'/gm))
    out.push({ method: m[1], pattern: m[2], re: new RegExp('^' + m[2].replace(/:\w+/g, '([^/]+)').replace(/\*/g, '.*') + '$', 'i') });
  return out;
}
export function pathnameOf(rawUrl) { try { return new URL(rawUrl, 'http://127.0.0.1').pathname; } catch { return null; } }
export function matchRoute(table, method, rawUrl) {
  const p = pathnameOf(rawUrl);
  return p == null ? null : table.find((r) => r.method === method && r.re.test(p)) || null;
}
