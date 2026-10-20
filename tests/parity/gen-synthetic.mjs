// Synthetic parity corpus: a scripted client talks to backend/server.mjs through recorder.mjs, one recorded session per
// scenario, into tests/parity/synthetic/<scenario>/. Covers every route of server.mjs (canonical path, mixed case and a
// method it does not serve), MCP locker and style flows, the matchmaker and XMPP WebSockets, odd paths and odd inputs
// (BOM, bad JSON, hotfix/cloud/ui files, --no-match). Only made-up data: safe to commit. Exits 1 if a route of
// server.mjs gets no canonical and mixed-case request, or a wrong-method request was routed.
// usage: node gen-synthetic.mjs [--server <server.mjs>] [--out <dir>] [--only <scenario>]
import fs from 'node:fs';
import os from 'node:os';
import net from 'node:net';
import path from 'node:path';
import crypto from 'node:crypto';
import { spawn } from 'node:child_process';
import { performance } from 'node:perf_hooks';
import { fileURLToPath } from 'node:url';
import { HttpConn, WsConn, requestBytes, wsFrame, sleep, routeTable, matchRoute, readSession, pathnameOf } from './wire.mjs';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const args = process.argv.slice(2);
const flag = (n, d) => { const i = args.indexOf(n); return i >= 0 ? args[i + 1] : d; };
const SERVER = path.resolve(flag('--server', path.join(HERE, '..', '..', 'backend', 'server.mjs')));
const OUT = path.resolve(flag('--out', path.join(HERE, 'synthetic')));
const ONLY = flag('--only', null);
const SRC = fs.readFileSync(SERVER, 'utf8');
const ROUTES = routeTable(SRC);

// ---------- made-up inputs ----------
const ACC = '0f1e2d3c4b5a69788796a5b4c3d2e1f0';
const SERVER_ID = crypto.createHash('md5').update('server' + ACC).digest('hex');
const UA = {
  '3.6': 'Fortnite/++Fortnite+Release-3.6-CL-4019403 Windows/10.0.19045.1.256.64bit',
  '14.30': 'Fortnite/++Fortnite+Release-14.30-CL-15341163 Windows/10.0.19045.1.256.64bit',
  '17.50': 'Fortnite/++Fortnite+Release-17.50-CL-17388565 Windows/10.0.19045.1.256.64bit',
};
const BOM = '\uFEFF';
const config = (o = {}) => JSON.stringify({ _help: { displayName: 'Your in-game name (random by default).' }, displayName: 'SynthPlayer', accountId: ACC, port: 3551, buildsFolder: 'S:\\FortniteBuilds', mode: 'practice', bots: 20, ...o }, null, 2);
const COSMETICS = JSON.stringify({ source: 'synthetic', build: '17.50', items: [
  { templateId: 'AthenaCharacter:CID_A_001_Athena_Commando_F_Synthetic', variants: [{ channel: 'Material', active: 'Mat1', owned: ['Mat1', 'Mat2', 'Mat3'] }, { channel: 'Parts', active: 'Stage1', owned: ['Stage1', 'Stage2'] }] },
  { templateId: 'AthenaCharacter:CID_A_002_Athena_Commando_M_Synthetic', variants: [] },
  { templateId: 'AthenaBackpack:BID_A_001_Synthetic', variants: [{ channel: 'Progressive', active: 'Stage1', owned: ['Stage1'] }] },
  { templateId: 'AthenaPickaxe:Pickaxe_A_001_Synthetic' },
  { templateId: 'AthenaGlider:Glider_A_001_Synthetic', variants: [] },
  { templateId: 'AthenaDance:EID_A_001_Synthetic', variants: [] },
  { templateId: 'AthenaDance:EID_DanceMoves', variants: [] },
  { templateId: 'AthenaItemWrap:Wrap_A_001_Synthetic', variants: [] },
  { templateId: 'AthenaSkyDiveContrail:Trails_A_001_Synthetic', variants: [] },
  { templateId: 'AthenaMusicPack:MusicPack_A_001_Synthetic', variants: [] },
  { templateId: 'AthenaLoadingScreen:LSID_A_001_Synthetic', variants: [] },
] }, null, 1);
const CUSTOM = JSON.stringify({ items: [
  { templateId: 'AthenaCharacter:CID_Custom_Synthetic', variants: [{ channel: 'Material', active: 'Mat0', owned: ['Mat0'] }] },
  { templateId: 'athenacharacter:cid_a_002_athena_commando_m_synthetic' }, // same item as the build's, other case: not added
  { variants: [] }, null,
], keychain: ['0123456789ABCDEF0123456789ABCDEF:c3ludGhldGljLWtleS0x:CID_Custom_Synthetic'] });
const KEYCHAIN = JSON.stringify(['FEDCBA9876543210FEDCBA9876543210:c3ludGhldGljLWtleS0y:BID_A_001_Synthetic']);
const STATE_OLD = JSON.stringify({ _help: 'kept as is by the backend', customKey: { a: [1, 2], b: null },
  slots: { Character: ['AthenaCharacter:CID_A_001_Athena_Commando_F_Synthetic'], Dance: ['', 'AthenaDance:EID_A_001_Synthetic'] },
  variantsActive: { 'athenacharacter:cid_a_001_athena_commando_f_synthetic': [{ channel: 'Material', active: 'Mat2', owned: ['Mat2'] }, { channel: 'Hidden', active: 'H1', owned: [] }] },
  favorites: { 'athenapickaxe:pickaxe_a_001_synthetic': true }, rvn: { athena: 7 } });
const JPG = Buffer.from('ffd8ffe000104a46494600010100000100010000ffd9', 'hex');
const PNG = Buffer.from('89504e470d0a1a0a0000000d4948445200000001000000010806000000', 'hex');
const SAV = Buffer.from([0x53, 0x41, 0x56, 0x00, 0x01, 0x02, 0xff, 0xfe, 0x10, 0x80]);
const inputsFull = {
  'offline/cosmetics.json': COSMETICS, 'offline/custom-cosmetics.json': CUSTOM, 'offline/keychain.json': KEYCHAIN,
  'offline/ui/br.jpg': JPG, 'offline/ui/cm.png': PNG, 'offline/ui/upper.PNG': PNG,
  'offline/cloud/ClientSettings.Sav': SAV,
};

// ---------- scripted client ----------
class Client {
  constructor(port, ua, name) { this.port = port; this.ua = ua; this.name = name; this.t0 = performance.now(); this.http = new HttpConn(port, () => this.now()); this.token = null; this.nws = 0; this.log = []; }
  now() { return performance.now() - this.t0; }
  headers(ua, extra = []) { const h = [['Host', `127.0.0.1:${this.port}`]]; if (ua !== null) h.push(['User-Agent', ua ?? this.ua]); h.push(['Accept', '*/*']); return [...h, ...extra]; }
  async req(method, url, { body, json, ctype, auth, ua, headers = [], conn } = {}) {
    let b = json !== undefined ? Buffer.from(typeof json === 'string' ? json : JSON.stringify(json)) : body == null ? null : Buffer.isBuffer(body) ? body : Buffer.from(body);
    const h = this.headers(ua, []);
    if (auth !== false && (auth || this.token)) h.push(['Authorization', auth || `bearer ${this.token}`]);
    if (b) h.push(['Content-Type', ctype || (json !== undefined ? 'application/json' : 'application/octet-stream')]);
    h.push(...headers);
    if (b || ['POST', 'PUT', 'PATCH'].includes(method)) h.push(['Content-Length', String(b?.length || 0)]);
    const m = await (conn || this.http).request(requestBytes(method, url, h, b || Buffer.alloc(0)), method);
    if (m.error) throw new Error(`${this.name}: ${method} ${url}: ${m.error}`);
    this.log.push({ method, url, status: m.status });
    return m;
  }
  async json(method, url, o) { const m = await this.req(method, url, o); try { return JSON.parse(m.body.toString()); } catch { return null; } }
  async login(username = 'SynthPlayer@offline.local') {
    const j = await this.json('POST', '/account/api/oauth/token', { body: `grant_type=password&username=${encodeURIComponent(username)}&password=offline&token_type=eg1`, ctype: 'application/x-www-form-urlencoded', auth: 'basic ZWM2ODRiOGM2ODdmNDc5ZmFkZWEzY2IyYWQ4M2Y1YzY6ZTFmMzFjMjExZjI4NDEzMTg2MjYyZDM3YTEzZmM4NGQ=' });
    this.token = j.access_token; this.account = j.account_id; return j;
  }
  mcp(command, profileId, body = {}, { rvn = -1, account = this.account || ACC, kind = 'client', raw } = {}) {
    return this.json('POST', `/fortnite/api/game/v2/profile/${account}/${kind}/${command}?profileId=${profileId}&rvn=${rvn}`, raw !== undefined ? { body: raw, ctype: 'application/json' } : { json: body });
  }
  // WebSocket: open with the given path/protocol; frames the server sends land in ws.frames
  async ws(url, { protocol, headers = [], upgrade = 'websocket' } = {}) {
    const key = crypto.createHash('md5').update(`${this.name}#${++this.nws}`).digest().toString('base64');
    const c = new WsConn(this.port, () => this.now(), Infinity);
    const h = this.headers(undefined, [['Upgrade', upgrade], ['Connection', 'Upgrade'], ['Sec-WebSocket-Key', key], ['Sec-WebSocket-Version', '13'], ...(protocol ? [['Sec-WebSocket-Protocol', protocol]] : []), ...headers]);
    await c.open(requestBytes('GET', url, h), 3000);
    return c;
  }
}
const until = async (pred, ms = 3000) => { const t = performance.now(); while (!pred()) { if (performance.now() - t > ms) return false; await sleep(10); } return true; };
const text = (s) => wsFrame(1, s);
const sendAndWait = async (c, bytes, n = 1, ms = 1500) => { const k = c.frames.length; c.send(bytes); await until(() => c.frames.length >= k + n, ms); await sleep(20); };
const plain = (user, pass = 'offline') => Buffer.from(`\0${user}\0${pass}`).toString('base64');
const XMPP_OPEN = '<open xmlns="urn:ietf:params:xml:ns:xmpp-framing" to="prod.ol.epicgames.com" version="1.0"/>';
async function xmppLogin(c, user, resource = 'V2:Fortnite:WIN::SYNTHETIC0001') {
  await sendAndWait(c, text(XMPP_OPEN), 2);
  await sendAndWait(c, text(`<auth xmlns="urn:ietf:params:xml:ns:xmpp-sasl" mechanism="PLAIN">${plain(user)}</auth>`));
  await sendAndWait(c, text(XMPP_OPEN), 2);
  await sendAndWait(c, text(`<iq type="set" id="_xmpp_bind1" xmlns="jabber:client"><bind xmlns="urn:ietf:params:xml:ns:xmpp-bind"><resource>${resource}</resource></bind></iq>`));
  await sendAndWait(c, text('<iq type="set" id="_xmpp_session1" xmlns="jabber:client"><session xmlns="urn:ietf:params:xml:ns:xmpp-session"/></iq>'));
}

// ---------- every route: canonical, mixed case, a method it does not serve ----------
const PARAMS = { id: ACC, name: 'SynthPlayer', svc: 'Fortnite', kind: 'client', command: 'QueryProfile', file: 'DefaultEngine.ini', sid: 'a0b1c2d3e4f5a6b7c8d9e0f1a2b3c4d5', code: 'playlist_defaultsolo', ns: 'Fortnite' };
const SPEC = {
  'POST /account/api/oauth/token': { form: 'grant_type=client_credentials&token_type=eg1' },
  'DELETE /account/api/oauth/sessions/kill*': { star: '', query: '?killType=OTHERS_ACCOUNT_CLIENT_SERVICE' },
  'GET /account/api/public/account': { query: `?accountId=${ACC}&accountId=${SERVER_ID}` },
  'POST /fortnite/api/game/v2/tryPlayOnPlatform/account/*': { star: ACC, query: '?platform=PC', body: null },
  'POST /fortnite/api/game/v2/grant_access/*': { star: ACC },
  'POST /fortnite/api/storeaccess/v1/request_access/*': { star: ACC },
  'GET /eulatracking/api/shared/agreements/fn*': { star: '', query: '?locale=en' },
  'GET /eulatracking/api/public/agreements/*': { star: `fn/account/${ACC}`, query: '?locale=en' },
  'GET /socialban/api/public/v1/*': { star: ACC },
  'GET /lightswitch/api/service/bulk/status': { query: '?serviceId=Fortnite' },
  'GET /fortnite/api/v2/versioncheck*': { star: '/Windows', query: '?version=%2B%2BFortnite%2BRelease-17.50-CL-17388565-Windows' },
  'GET /fortnite/api/versioncheck*': { star: '', query: '?version=%2B%2BFortnite%2BRelease-3.6-CL-4019403-Windows' },
  'GET /content/api/pages/fortnite-game*': { star: '' },
  'GET /fortnite/api/cloudstorage/user/:id/:file': { params: { file: 'ClientSettings.Sav' } },
  'PUT /fortnite/api/cloudstorage/user/:id/:file': { params: { file: 'Synthetic.Sav' }, body: Buffer.from([1, 2, 3, 0, 255]) },
  'POST /fortnite/api/game/v2/profile/:id/:kind/:command': { query: '?profileId=athena&rvn=-1' },
  'GET /fortnite/api/storefront/v2/gift/check_eligibility/*': { star: `recipient/${ACC}/offer/v2%3A%2Fsynthetic` },
  'POST /fortnite/api/game/v2/privacy/account/:id': { json: { optOutOfPublicLeaderboards: false } },
  'GET /fortnite/api/game/v2/events/*': { star: `tournamentandhistory/${ACC}/EU/WindowsClient` },
  'GET /api/v1/events/Fortnite/download/:id*': { query: '?region=EU&platform=Windows&teamAccountIds=' + ACC },
  'GET /api/v1/events/Fortnite/*': { star: `data/${ACC}` },
  'POST /fortnite/api/statsv2/query': { json: { appId: 'fortnite', startDate: 0, endDate: 0, owners: [ACC], stats: [] } },
  'POST /statsproxy/api/statsv2/query': { json: { appId: 'fortnite', owners: [ACC], stats: ['br_placetop1_keyboardmouse_m0_playlist_defaultsolo'] } },
  'GET /fortnite/api/game/v2/leaderboards/*': { star: `cohort/${ACC}` },
  'GET /fortnite/api/game/v2/creative/*': { star: `favorites/${ACC}`, query: '?limit=30' },
  'GET /fortnite/api/game/v2/twitch/*': { star: ACC },
  'POST /fortnite/api/feedback/*': { star: 'Bug', body: 'synthetic feedback', ctype: 'text/plain' },
  'POST /fortnite/api/game/v2/chat/*': { star: `${ACC}/recommendGeneralChatRooms/pc` },
  'POST /offline/server-ready': { json: { playlist: 'playlist_defaultsolo', port: 7777, code: 'playlist_defaultsolo' } },
  'POST /offline/server-down': { body: null },
  'GET /fortnite/api/game/v2/matchmakingservice/ticket/player/*': { star: ACC, query: `?partyPlayerIds=${ACC}&bucketId=17388565%3A1%3AEU%3Aplaylist_defaultsolo&player.platform=Windows&player.option.crossplayOptOut=false` },
  'POST /fortnite/api/matchmaking/session/:sid/join': { query: `?accountId=${ACC}`, body: null },
  'POST /fortnite/api/matchmaking/session/matchMakingRequest': { json: { criteria: [], openPlayersRequired: 1, buildUniqueId: '17388565', maxResults: 1 } },
  'GET /fortnite/api/matchmaking/session/findPlayer/*': { star: ACC },
  'GET /fortnite/api/discovery/accessToken/*': { star: '++Fortnite+Release-17.50' },
  'POST /fortnite/api/game/v2/creative/discovery/surface/*': { star: ACC, query: '?appId=Fortnite', json: { surfaceName: 'CreativeDiscoverySurface_Frontend', revision: -1, partyMemberIds: [ACC], matchmakingRegion: 'EU' } },
  'POST /discovery/surface/*': { star: 'CreativeDiscoverySurface_Frontend', query: '?appId=Fortnite', json: { playerId: ACC, partyMemberIds: [ACC], locale: 'en', matchmakingRegion: 'EU', platform: 'Windows', isCabined: false, ratingAuthority: '', rating: '', numLocalPlayers: 1 } },
  'POST /api/v1/assets/Fortnite/*': { star: '++Fortnite+Release-17.50/17388565', json: { FortCreativeDiscoverySurface: 0, FortPlaylistAthena: 0 } },
  'POST /links/api/fn/mnemonic': { json: [{ mnemonic: 'playlist_defaultsolo', type: '', filter: false, v: '' }, { mnemonic: 'playlist_playground', type: '', filter: false, v: '' }] },
  'POST /api/v1/links/lock-status/:id/check': { json: { linkCodes: ['playlist_defaultsolo', 'playlist_playground', 'playlist_unknown'] } },
  'GET /offline/ui/:file': { params: { file: 'br.jpg' } },
  'POST /party/api/v1/Fortnite/parties': { json: { config: { join_confirmation: false, joinability: 'OPEN', max_size: 16 }, join_info: { connection: { id: `${ACC}@prod.ol.epicgames.com/V2:Fortnite:WIN::SYNTHETIC0001`, meta: { 'urn:epic:conn:platform_s': 'WIN', 'urn:epic:conn:type_s': 'game' } }, meta: { 'urn:epic:member:dn_s': 'SynthPlayer' } }, meta: { 'urn:epic:cfg:party-type-id_s': 'default', 'urn:epic:cfg:build-id_s': '1:3:17388565' } } },
  'PATCH /party/api/v1/Fortnite/parties/*': { star: `b0c1d2e3f4a5b6c7d8e9f0a1b2c3d4e5/members/${ACC}/meta`, json: { delete: [], revision: 1, update: { 'Default:Location_s': 'PreLobby' } } },
  'DELETE /party/api/v1/Fortnite/parties/*': { star: `b0c1d2e3f4a5b6c7d8e9f0a1b2c3d4e5/members/${ACC}` },
  'POST /party/api/v1/Fortnite/parties/*': { star: `b0c1d2e3f4a5b6c7d8e9f0a1b2c3d4e5/members/${ACC}/join`, json: { connection: { id: 'x' } } },
  'GET /presence/api/v1/*': { star: `_/${ACC}/subscriptions/nudged` },
  'POST /presence/api/v1/*': { star: `Fortnite/${ACC}/subscriptions/broadcast`, json: {} },
  'POST /datarouter/api/v1/public/data*': { star: '', query: '?SessionID=%7BSYNTHETIC%7D&AppID=FortniteGame&AppVersion=%2B%2BFortnite%2BRelease-17.50-CL-17388565&UserID=' + ACC, json: { Events: [{ EventName: 'Core.Loading', Seconds: 1.5 }] } },
  'POST /telemetry/*': { star: 'data/datarouter/api/v1/public/data', json: { Events: [] } },
  'GET /content-controls/*': { star: ACC },
  'POST /content-controls/*': { star: `${ACC}/verify-pin`, json: { pin: '' } },
  'GET /hotconfigs/v2/*': { star: 'livefn.json' },
  'POST /api/v1/user/setting': { json: { accountIds: [ACC], settings: ['avatar', 'avatarBackground', 'appInstalled'] } },
  'GET /fortnite/api/game/v2/homebase/*': { star: 'allowed-name-chars' },
  'GET /affiliate/api/public/affiliates/slug/*': { star: 'synthetic' },
};
const swapCase = (c) => (c === c.toLowerCase() ? c.toUpperCase() : c.toLowerCase());
function sampleRequest(r, mixed = false) {
  const s = SPEC[`${r.method} ${r.pattern}`] || {};
  const params = { ...PARAMS, ...(s.params || {}) };
  let i = 0;
  const p = r.pattern.split('/').map((seg) => {
    if (seg.startsWith(':')) { const m = /^:(\w+)(\*?)$/.exec(seg); return params[m[1]] + (m[2] ? (s.star ?? '') : ''); }
    const lit = seg.replace(/[A-Za-z]/g, (c) => (mixed && i++ % 2 === 0 ? swapCase(c) : c));
    return lit.replace(/\*$/, () => s.star ?? 'any/thing').replace(/:(\w+)\*$/, (_, k) => params[k]);
  }).join('/');
  const o = {};
  if (s.form) { o.body = s.form; o.ctype = 'application/x-www-form-urlencoded'; }
  else if (s.json !== undefined) o.json = s.json;
  else if (s.body !== undefined) { if (s.body !== null) { o.body = s.body; o.ctype = s.ctype; } }
  else if (['POST', 'PUT', 'PATCH'].includes(r.method)) o.json = {};
  return { url: p + (s.query ?? ''), o };
}

// ---------- scenarios ----------
const SCENARIOS = [];
const scenario = (name, def) => SCENARIOS.push({ name, ...def });

scenario('routes-17.50', {
  build: '17.50', ua: UA['17.50'], config: config(), files: inputsFull,
  async run(c, ctx) {
    await c.login();
    for (const r of ROUTES) {
      const canon = sampleRequest(r), mixed = sampleRequest(r, true);
      ctx.expect.push({ route: r, method: r.method, url: canon.url, kind: 'canonical' }); await c.req(r.method, canon.url, canon.o);
      ctx.expect.push({ route: r, method: r.method, url: mixed.url, kind: 'mixed-case' }); await c.req(r.method, mixed.url, mixed.o);
      const p = pathnameOf(canon.url);
      const wrong = ['GET', 'POST', 'PUT', 'DELETE', 'PATCH', 'OPTIONS'].find((m) => !ROUTES.some((x) => x.method === m && x.re.test(p)));
      if (wrong) { ctx.expect.push({ route: null, method: wrong, url: canon.url, kind: 'wrong-method' }); await c.req(wrong, canon.url, ['POST', 'PUT', 'PATCH'].includes(wrong) ? { json: {} } : {}); }
    }
    // param case matters where server.mjs compares or lower-cases the value
    await c.req('GET', `/account/api/public/account/${SERVER_ID.toUpperCase()}`);
    await c.req('GET', `/account/api/public/account/${SERVER_ID}`);
    await c.req('GET', '/links/api/fn/mnemonic/Playlist_DefaultSquad');
    await c.req('GET', '/links/api/fn/mnemonic/PLAYLIST_TRIOS/related');
    await c.req('GET', '/links/api/fn/mnemonic/playlist_nope');
    await c.req('GET', '/links/api/fn/mnemonic/playlist_nope/related');
    await c.req('GET', '/fortnite/api/cloudstorage/system/defaultengine.ini');
    await c.req('GET', '/fortnite/api/cloudstorage/system/Missing.ini');
    await c.req('GET', '/fortnite/api/cloudstorage/user/' + ACC + '/Missing.Sav');
    await c.req('GET', '/offline/ui/cm.png'); await c.req('GET', '/offline/ui/upper.PNG'); await c.req('GET', '/offline/ui/missing.jpg');
  },
});

scenario('mcp-styles-17.50', {
  build: '17.50', ua: UA['17.50'], config: config(), files: inputsFull,
  async run(c) {
    await c.login();
    for (const p of ['athena', 'common_core', 'common_public', 'creative', 'collections', 'campaign']) await c.mcp('QueryProfile', p);
    await c.mcp('ClientQuestLogin', 'athena', { streamingAppKey: '' });
    await c.mcp('EquipBattleRoyaleCustomization', 'athena', { slotName: 'Character', itemToSlot: 'AthenaCharacter:CID_A_001_Athena_Commando_F_Synthetic', indexWithinSlot: 0, variantUpdates: [{ channel: 'Material', active: 'Mat3', owned: [] }, { channel: 'Parts', active: 'Stage2' }] });
    await c.mcp('EquipBattleRoyaleCustomization', 'athena', { slotName: 'Character', itemToSlot: 'AthenaCharacter:CID_Custom_Synthetic', indexWithinSlot: 0, variantUpdates: [{ channel: 'Material', active: 'Mat5', owned: ['Mat5'] }, { channel: 'Pattern', active: 'P2' }] });
    await c.mcp('EquipBattleRoyaleCustomization', 'athena', { slotName: 'Backpack', itemToSlot: 'AthenaBackpack:BID_A_001_Synthetic', indexWithinSlot: 0, variantUpdates: [null, { channel: '' }, { channel: 'Progressive' }, { channel: 'Progressive', active: 'Stage3' }] });
    await c.mcp('EquipBattleRoyaleCustomization', 'athena', { slotName: 'Dance', itemToSlot: 'AthenaDance:EID_A_001_Synthetic', indexWithinSlot: 3 });
    await c.mcp('EquipBattleRoyaleCustomization', 'athena', { slotName: 'ItemWrap', itemToSlot: 'AthenaItemWrap:Wrap_A_001_Synthetic', indexWithinSlot: -1 });
    await c.mcp('SetCosmeticLockerSlot', 'athena', { lockerItem: 'sandbox_loadout', category: 'ItemWraps', slotIndex: 2, itemToSlot: '', variantUpdates: [] });
    await c.mcp('SetCosmeticLockerSlot', 'athena', { lockerItem: 'sandbox_loadout', category: 'Pickaxe', slotIndex: 0, itemToSlot: 'AthenaPickaxe:Pickaxe_A_001_Synthetic', variantUpdates: [] });
    await c.mcp('SetCosmeticLockerSlot', 'athena', { lockerItem: 'sandbox_loadout', category: 'Glider', itemToSlot: 'athenaglider:GLIDER_a_001_synthetic', variantUpdates: [{ channel: 'Material', active: 'Mat1' }] });
    await c.mcp('SetCosmeticLockerSlot', 'athena', { lockerItem: 'sandbox_loadout', category: 'Hat', slotIndex: 0, itemToSlot: 'AthenaHat:Nope' });
    await c.mcp('SetCosmeticLockerSlot', 'athena', { category: 'SkyDiveContrail', itemToSlot: 'AthenaSkyDiveContrail:Trails_A_001_Synthetic' });
    await c.mcp('SetCosmeticLockerSlot', 'athena', { category: 'MusicPack', itemToSlot: 'AthenaMusicPack:MusicPack_A_001_Synthetic' });
    await c.mcp('SetCosmeticLockerSlot', 'athena', { category: 'LoadingScreen', itemToSlot: 'AthenaLoadingScreen:LSID_A_001_Synthetic' });
    await c.mcp('SetCosmeticLockerBanner', 'athena', { lockerItem: 'sandbox_loadout', bannerIconTemplateName: 'BRSeason03', bannerColorTemplateName: 'DefaultColor15' });
    await c.mcp('SetBattleRoyaleBanner', 'athena', { homebaseBannerIconId: 'x', bannerIconTemplateName: '' });
    await c.mcp('SetCosmeticLockerName', 'athena', { lockerItem: 'sandbox_loadout', name: 'Ñandú locker ✓' });
    await c.mcp('SetItemFavoriteStatus', 'athena', { targetItemId: 'AthenaPickaxe:Pickaxe_A_001_Synthetic', bFavorite: true });
    await c.mcp('SetItemFavoriteStatus', 'athena', { bFavorite: true });
    await c.mcp('SetItemFavoriteStatusBatch', 'athena', { itemIds: ['AthenaGlider:Glider_A_001_Synthetic', 'AthenaDance:EID_A_001_Synthetic'], itemFavStatus: [true] });
    await c.mcp('MarkItemSeen', 'athena', { itemIds: ['AthenaGlider:Glider_A_001_Synthetic'] });
    await c.mcp('SetMtxPlatform', 'common_core', { newPlatform: 'EpicPC' });
    await c.mcp('EquipBattleRoyaleCustomization', 'common_core', { slotName: 'Glider', itemToSlot: 'AthenaGlider:Glider_A_001_Synthetic' });
    await c.mcp('FooBarUnknownCommand', 'athena', { x: 1 });
    await c.mcp('EquipBattleRoyaleCustomization', 'athena', {}, { raw: '{"slotName": "Character", ' });
    await c.mcp('EquipBattleRoyaleCustomization', 'athena', {}, { raw: '' });
    await c.json('POST', `/fortnite/api/game/v2/profile/${ACC}/client/QueryProfile`, { json: {} });
    await c.json('POST', `/fortnite/api/game/v2/profile/${ACC}/client/QueryProfile?profileId=athena&rvn=abc`, { json: {} });
    await c.json('POST', `/fortnite/api/game/v2/profile/${ACC}/dedicated_server/QueryProfile?profileId=athena&rvn=12`, { json: {} });
    await c.mcp('QueryProfile', 'athena');
  },
});

function lobby(build) {
  return {
    build, ua: UA[build], config: config(), files: { 'offline/cosmetics.json': COSMETICS, 'offline/cloud/ClientSettings.Sav': SAV },
    async run(c) {
      await c.req('GET', '/fortnite/api/version', { auth: false });
      await c.req('GET', `/fortnite/api/v2/versioncheck/Windows?version=${encodeURIComponent(`++Fortnite+Release-${build}-Windows`)}`, { auth: false });
      await c.login();
      await c.req('GET', '/account/api/oauth/verify');
      await c.req('GET', `/account/api/public/account/${ACC}`);
      await c.req('GET', `/account/api/public/account/${ACC}/externalAuths`);
      await c.req('GET', '/fortnite/api/game/v2/enabled_features');
      await c.req('GET', '/lightswitch/api/service/bulk/status?serviceId=Fortnite');
      await c.req('GET', '/fortnite/api/cloudstorage/system');
      await c.req('GET', '/fortnite/api/cloudstorage/system/DefaultEngine.ini');
      await c.req('GET', `/fortnite/api/cloudstorage/user/${ACC}`);
      await c.req('GET', `/fortnite/api/cloudstorage/user/${ACC}/ClientSettings.Sav`);
      await c.req('GET', '/fortnite/api/calendar/v1/timeline');
      await c.req('GET', '/content/api/pages/fortnite-game');
      for (const p of ['athena', 'common_core', 'common_public']) await c.mcp('QueryProfile', p);
      await c.mcp('ClientQuestLogin', 'athena');
      await c.req('GET', '/fortnite/api/storefront/v2/catalog');
      await c.req('GET', '/fortnite/api/storefront/v2/keychain?numKeysDownloaded=0');
      await c.req('GET', `/friends/api/public/friends/${ACC}?includePending=true`);
      await c.req('GET', `/friends/api/v1/${ACC}/summary`);
      await c.req('GET', `/party/api/v1/Fortnite/user/${ACC}`);
      const x = await c.ws('/xmpp', { protocol: 'xmpp' });
      await xmppLogin(x, ACC);
      await sendAndWait(x, text(`<presence><status>{"Status":"Battle Royale Lobby - 1 / 16","bIsPlaying":false,"bIsJoinable":false,"SessionId":"","Properties":{}}</status><delay xmlns="urn:xmpp:delay" stamp="2026-10-20T10:00:00.000Z"/></presence>`));
      await c.mcp('EquipBattleRoyaleCustomization', 'athena', { slotName: 'Character', itemToSlot: 'AthenaCharacter:CID_A_002_Athena_Commando_M_Synthetic', indexWithinSlot: 0 });
      await sleep(300);
      await sendAndWait(x, text('<close xmlns="urn:ietf:params:xml:ns:xmpp-framing"/>'), 2);
      x.destroy();
      await c.req('DELETE', `/account/api/oauth/sessions/kill/${c.token}`);
    },
  };
}
scenario('lobby-3.6', lobby('3.6'));
scenario('lobby-14.30', lobby('14.30'));

scenario('seasons-ua', {
  build: '17.50', ua: UA['17.50'], config: config({ accountId: ACC.toUpperCase(), displayName: 'Seasons' }), files: {},
  async run(c) {
    const uas = [null, 'Fortnite/++Fortnite+Release-1.7.2-CL-3700114 Windows/10.0.17134.1.768.64bit', 'Fortnite/++Fortnite+Release-1.8-CL-3724489 Windows/10',
      'Fortnite/++Fortnite+Release-1.8-CL-3724490 Windows/10', 'Fortnite/++Fortnite+Release-2.5-CL-3889387 Windows/10', UA['3.6'],
      'Fortnite/++Fortnite+Release-10.40-CL-9380822 Windows/10', UA['14.30'], UA['17.50'], 'Fortnite/++Fortnite+Release-Cert-CL-123 Windows/10',
      'fortnite/++fortnite+release-12.41-cl-12905909 windows/10', 'UELauncher/1.0'];
    const j = await c.login();
    const srv = await c.json('POST', '/account/api/oauth/token', { body: 'grant_type=password&username=server%40offline.local&password=x', ctype: 'application/x-www-form-urlencoded', auth: false });
    await c.req('GET', '/account/api/oauth/verify', { auth: `bearer ${srv.access_token}` });
    await c.req('GET', '/account/api/oauth/verify', { auth: 'bearer eg1~offline-ffffffffffffffffffffffffffffffff' });
    await c.req('GET', '/account/api/oauth/verify', { auth: false });
    await c.req('GET', `/account/api/public/account/${srv.account_id}`);
    await c.req('GET', `/account/api/public/account?accountId=${srv.account_id}&accountId=${j.account_id}&accountId=`);
    await c.req('GET', '/account/api/public/account');
    let n = 0;
    for (const ua of uas) {
      n++;
      await c.req('GET', '/fortnite/api/calendar/v1/timeline', { ua });
      await c.req('GET', '/content/api/pages/fortnite-game', { ua });
      await c.mcp('QueryProfile', 'athena', {}, { account: j.account_id, rvn: n });
      await c.req('GET', `/synthetic/unknown/ua-${n}`, { ua });
    }
    for (const ua of uas.slice(1, 6)) await c.req('POST', `/fortnite/api/game/v2/profile/${j.account_id}/client/QueryProfile?profileId=athena`, { json: {}, ua });
  },
});

scenario('match-17.50', {
  build: '17.50', ua: UA['17.50'], config: config(), files: { 'offline/cosmetics.json': COSMETICS },
  async run(c) {
    await c.login();
    await c.req('GET', '/offline/wanted');
    await c.req('GET', `/fortnite/api/game/v2/matchmakingservice/ticket/player/${ACC}?partyPlayerIds=${ACC}&bucketId=17388565%3A1%3AEU%3Aplaylist_defaultsolo&player.platform=Windows&player.subregions=GB&player.option.crossplayOptOut=false`);
    const w = await c.ws('/matchmaking', { headers: [['Authorization', 'Epic-Signed mms-player offline offline']] });
    await sleep(1300);
    await c.req('GET', '/offline/wanted');
    await sleep(1200);
    await c.req('POST', '/offline/server-ready', { json: { playlist: 'playlist_defaultsolo', port: 7777, code: 'playlist_defaultsolo' } });
    await until(() => w.frames.some((f) => /"Play"/.test(f.body || '')), 4000);
    const play = JSON.parse(w.frames.find((f) => /"Play"/.test(f.body || '')).body);
    await until(() => w.serverEnd != null, 1500);
    w.destroy();
    await c.req('GET', `/fortnite/api/game/v2/matchmaking/account/${ACC}/session/${play.payload.sessionId}`);
    await c.req('GET', `/fortnite/api/matchmaking/session/${play.payload.sessionId}`);
    await c.req('POST', `/fortnite/api/matchmaking/session/${play.payload.sessionId}/join?accountId=${ACC}`);
    await c.req('GET', '/offline/wanted');
    // a server that was ready before the ticket is not reused: the client stays queued, then cancels
    await c.req('POST', '/offline/server-ready', { json: { playlist: 'playlist_defaultduo', port: 7778 } });
    await sleep(50);
    await c.req('GET', `/fortnite/api/game/v2/matchmakingservice/ticket/player/${ACC}?bucketId=17388565%3A1%3AEU%3APlaylist_DefaultDuo`);
    const w2 = await c.ws('/matchmaking');
    await sleep(1400);
    await c.req('GET', '/offline/wanted');
    await sleep(800);
    w2.send(wsFrame(8, Buffer.from([0x03, 0xe8])));
    await until(() => w2.closed != null, 1500);
    w2.destroy();
    await c.req('POST', '/offline/server-down');
    await c.req('GET', '/offline/wanted');
    await c.req('GET', `/fortnite/api/matchmaking/session/${'c'.repeat(32)}`);
    // bucket ids the matchmaker cannot read, a bad server-ready body
    await c.req('GET', `/fortnite/api/game/v2/matchmakingservice/ticket/player/${ACC}?bucketId=abc%3A1%3AEU%3Aplaylist_trios`);
    await c.req('GET', `/fortnite/api/game/v2/matchmakingservice/ticket/player/${ACC}`);
    await c.req('POST', '/offline/server-ready', { body: '{not json', ctype: 'application/json' });
    await c.req('GET', '/offline/wanted');
    await c.req('GET', `/fortnite/api/matchmaking/session/${'d'.repeat(32)}`);
    // the client goes away without a close frame while queued
    const w3 = await c.ws('/matchmaking');
    await sleep(1150);
    w3.end(); await sleep(100); w3.destroy();
    await sleep(1200);
    await c.req('POST', '/offline/server-down');
  },
});

scenario('no-match-14.30', {
  build: '14.30', ua: UA['14.30'], flags: ['--no-match'], config: config(), files: {},
  async run(c) {
    await c.login();
    await c.req('GET', `/fortnite/api/game/v2/matchmakingservice/ticket/player/${ACC}?bucketId=15341163%3A1%3AEU%3Aplaylist_defaultsolo`);
    await c.req('GET', '/offline/wanted');
    const w = await c.ws('/matchmaking');
    await sleep(1300);
    w.send(wsFrame(8, Buffer.from([0x03, 0xe8])));
    await until(() => w.closed != null, 1500);
    w.destroy();
  },
});

scenario('xmpp-17.50', {
  build: '17.50', ua: UA['17.50'], config: config(), files: {},
  async run(c) {
    await c.login();
    const x = await c.ws('/xmpp', { protocol: 'xmpp' });
    await xmppLogin(x, ACC);
    await sendAndWait(x, text('<presence><status>{"Status":"Playing"}</status></presence>'));
    await sendAndWait(x, text('<presence to="party-synthetic@muc.prod.ol.epicgames.com/SynthPlayer:0f1e:V2:Fortnite:WIN::X"><x xmlns="http://jabber.org/protocol/muc"/></presence>'));
    await sendAndWait(x, text('<presence type="unavailable" to="a@b"/>'));
    x.send(text('<message to="friend@prod.ol.epicgames.com" type="chat"><body>hi</body></message>')); await sleep(200);
    await sendAndWait(x, text('<iq id="ping_1" type="get" to="prod.ol.epicgames.com" xmlns="jabber:client"><ping xmlns="urn:xmpp:ping"/></iq>'));
    await sendAndWait(x, text("<iq id='single_quoted' type='get'><query xmlns='jabber:iq:roster'/></iq>"));
    await sendAndWait(x, wsFrame(9, 'ping-1'));
    x.send(wsFrame(10, 'pong-unsolicited')); x.send(wsFrame(2, Buffer.from([1, 2, 3]))); await sleep(150);
    await sendAndWait(x, text(`<presence><status>${'S'.repeat(300)}</status></presence>`));
    await sendAndWait(x, text(`<presence><status>${'L'.repeat(65600)}</status></presence>`), 1, 3000);
    await sendAndWait(x, wsFrame(1, '<presence><status>unmasked</status></presence>', { mask: false }));
    await sendAndWait(x, Buffer.concat([wsFrame(1, '<presence><status>frag', { fin: false }), wsFrame(0, 'ment</status></presence>')]));
    await sendAndWait(x, Buffer.concat([text('<iq id="two_a" type="get"/>'), text('<iq id="two_b" type="get"/>')]), 2);
    const split = text('<iq id="split_frame" type="get"/>');
    x.send(split.subarray(0, 5)); await sleep(80); await sendAndWait(x, split.subarray(5));
    x.send(text('just text, not xml')); await sleep(150);
    await sendAndWait(x, text('<close xmlns="urn:ietf:params:xml:ns:xmpp-framing"/>'), 2);
    await until(() => x.serverEnd != null, 1000); x.destroy();
    // by protocol on another path, a protocol list, a user that is not an account id, a bad auth, a client close frame
    const y = await c.ws('/', { protocol: 'chat, xmpp' });
    await sendAndWait(y, text(XMPP_OPEN), 2);
    await sendAndWait(y, text(`<auth xmlns="urn:ietf:params:xml:ns:xmpp-sasl" mechanism="PLAIN">${plain('SynthPlayer@offline.local')}</auth>`));
    await sendAndWait(y, text('<iq type="set" id="b2"><bind xmlns="urn:ietf:params:xml:ns:xmpp-bind"/></iq>'));
    await sendAndWait(y, text('<auth mechanism="PLAIN"></auth>'));
    await sendAndWait(y, text('<iq type="set" id="b3"><bind xmlns="urn:ietf:params:xml:ns:xmpp-bind"><resource>R</resource></bind></iq>'));
    y.send(wsFrame(8, Buffer.concat([Buffer.from([0x03, 0xe8]), Buffer.from('bye')])));
    await until(() => y.closed != null, 1500); y.destroy();
    // /matchmaking with the xmpp protocol is XMPP; /xmpp without a protocol header is XMPP without one in the answer
    const z = await c.ws('/matchmaking?x=1', { protocol: 'xmpp' }); await sendAndWait(z, text(XMPP_OPEN), 2); z.destroy();
    const v = await c.ws('/xmpp/sub?q=1'); await sendAndWait(v, text(XMPP_OPEN), 2); v.destroy();
    // upgrades the backend refuses (closes without an answer): case-sensitive paths, other paths, an HTTP route
    for (const u of ['/XMPP', '/Matchmaking', '/foo', '/offline/health']) { const r = await c.ws(u); r.destroy(); }
    const m = await c.ws('/matchmakingfoo'); await sleep(700); m.destroy();
    await c.req('GET', '/offline/health');
  },
});

scenario('inputs-bom-17.50', {
  build: '17.50', ua: UA['17.50'],
  config: BOM + config({ displayName: 'Ñandú_ThisNameIsLongerThan32Characters_Cut' }),
  files: {
    'offline/cosmetics.json': BOM + COSMETICS, 'offline/custom-cosmetics.json': BOM + CUSTOM, 'offline/keychain.json': BOM + KEYCHAIN,
    'offline/state.json': BOM + STATE_OLD,
    'offline/hotfix/DefaultEngine.ini': BOM + '[/Script/Engine.GameEngine]\r\n+NetDriverDefinitions=(DefName="GameNetDriver")\r\n',
    'offline/hotfix/DefaultRuntimeOptions.ini': '[/Script/FortniteGame.FortRuntimeOptions]\r\nbSkipSubgameSelect=true\r\n',
    'offline/hotfix/Upper.INI': '[Upper]\r\nKey=1\r\n',
    'offline/hotfix/notes.txt': 'not an ini',
    'offline/cloud/ClientSettings.Sav': SAV, 'offline/cloud/Other.sav': Buffer.from('other'),
    'offline/ui/br.jpg': JPG, 'offline/ui/cm.png': PNG,
  },
  async run(c) {
    await c.req('GET', '/offline/health');
    await c.login();
    await c.req('GET', `/account/api/public/account/${ACC}`);
    await c.mcp('QueryProfile', 'athena');
    await c.mcp('QueryProfile', 'common_public');
    await c.req('GET', '/fortnite/api/storefront/v2/keychain');
    await c.req('GET', '/fortnite/api/cloudstorage/system');
    for (const f of ['DefaultEngine.ini', 'DefaultRuntimeOptions.ini', 'Upper.INI', 'notes.txt', 'DefaultEngine%2Eini']) await c.req('GET', `/fortnite/api/cloudstorage/system/${f}`);
    await c.req('GET', `/fortnite/api/cloudstorage/user/${ACC}`);
    await c.mcp('EquipBattleRoyaleCustomization', 'athena', { slotName: 'Character', itemToSlot: 'AthenaCharacter:CID_A_001_Athena_Commando_F_Synthetic', variantUpdates: [{ channel: 'Parts', active: 'Stage2' }] });
    await c.mcp('SetItemFavoriteStatus', 'athena', { targetItemId: 'AthenaCharacter:CID_Custom_Synthetic', bFavorite: true });
    await c.mcp('QueryProfile', 'athena');
  },
});

scenario('inputs-bad-json-3.6', {
  build: '3.6', ua: UA['3.6'], config: '{"displayName": "Broken", "accountId": ',
  files: { 'offline/cosmetics.json': '{"items": [', 'offline/custom-cosmetics.json': 'nope', 'offline/state.json': '{"slots": {"Character": [', 'offline/keychain.json': '{"not": "an array"}' },
  async run(c) {
    await c.req('GET', '/offline/health');
    const j = await c.login();
    await c.json('POST', '/account/api/oauth/token', { body: 'grant_type=password&username=Server%40Offline.local', ctype: 'application/x-www-form-urlencoded', auth: false });
    await c.req('GET', `/account/api/public/account/${j.account_id}`);
    await c.mcp('QueryProfile', 'athena', {}, { account: j.account_id });
    await c.mcp('EquipBattleRoyaleCustomization', 'athena', { slotName: 'Character', itemToSlot: 'AthenaCharacter:CID_A_001_Athena_Commando_F_Synthetic', variantUpdates: [{ channel: 'Material', active: 'Mat2' }] }, { account: j.account_id });
    await c.req('GET', '/fortnite/api/storefront/v2/keychain');
    await c.req('POST', '/party/api/v1/Fortnite/parties', { body: '{bad', ctype: 'application/json' });
    await c.req('POST', `/api/v1/links/lock-status/${j.account_id}/check`, { body: 'nope', ctype: 'application/json' });
    await c.req('POST', `/api/v1/links/lock-status/${j.account_id}/check`, { json: { linkCodes: 'not-an-array' } });
  },
});

scenario('paths-edge-17.50', {
  build: '17.50', ua: UA['17.50'], config: config(), files: { 'offline/cloud/ClientSettings.Sav': SAV, 'offline/ui/br.jpg': JPG },
  async run(c) {
    await c.login();
    const paths = [
      '/offline/health/', '/offline//health', '/offline/./health', '/offline/x/../health', '/offline/%2e%2e/offline/health', '/offline\\health',
      '/offline/health?x=1&y', '/offline/health#fragment', '/offline/%68ealth', '//offline/health', 'http://example.com/offline/health',
      '/account/api/public/account/abc%2Fdef', '/account/api/public/account/displayName/Synth%20Player', `/account/api/public/account/${ACC}/externalAuths/`,
      '/fortnite/api/cloudstorage/system/%ZZ', `/fortnite/api/cloudstorage/user/${ACC}/%E0%A4%A`, '/fortnite/api/cloudstorage/system/', '/fortnite/api/cloudstorage/system/config',
      '/offline/ui/caf%C3%A9.png', '/offline/ui/..%2Fcloud%2FClientSettings.Sav', '/content/api/pages/fortnite-game/spark-tracks', '/fortnite/api/versioncheckXYZ',
      '/synthetic/unknown', '/synthetic/unknown', '/Synthetic/Unknown', `/${'long/'.repeat(300)}end`,
    ];
    for (const p of paths) await c.req('GET', p);
    await c.req('HEAD', '/offline/health');
    await c.req('OPTIONS', '/offline/health');
    await c.req('POST', '/offline/health', { json: {} });
    await c.req('DELETE', '/synthetic/unknown');
    await c.req('GET', `/account/api/oauth/sessions/kill/${c.token}`);
    // cloud files: path traversal is cut to the file name; 4 MiB is not stored (limit is < 4 MiB)
    await c.req('PUT', `/fortnite/api/cloudstorage/user/${ACC}/..%2F..%2Fsneaky.sav`, { body: Buffer.from('traversal') });
    await c.req('PUT', `/fortnite/api/cloudstorage/user/${ACC}/Empty.sav`, { body: Buffer.alloc(0) });
    await c.req('PUT', `/fortnite/api/cloudstorage/user/${ACC}/Big.sav`, { body: Buffer.alloc(4 * 1024 * 1024, 'A') });
    await c.req('GET', `/fortnite/api/cloudstorage/user/${ACC}/sneaky.sav`);
    await c.req('GET', `/fortnite/api/cloudstorage/user/${ACC}/Big.sav`);
    await c.req('GET', `/fortnite/api/cloudstorage/user/${ACC}`);
    // chunked request body, HTTP/1.0, a request with no User-Agent
    const chunked = Buffer.from('7\r\n{"a":1,\r\n6\r\n"b":2}\r\n0\r\n\r\n');
    await c.http.request(requestBytes('POST', `/api/v1/links/lock-status/${ACC}/check`, [['Host', `127.0.0.1:${c.port}`], ['User-Agent', c.ua], ['Transfer-Encoding', 'chunked'], ['Content-Type', 'application/json']], chunked), 'POST');
    await c.http.request(requestBytes('GET', '/offline/health', [['Host', `127.0.0.1:${c.port}`]], Buffer.alloc(0), 'HTTP/1.0'), 'GET');
    await c.req('GET', '/fortnite/api/calendar/v1/timeline', { ua: null });
  },
});

// ---------- run ----------
const freePort = () => new Promise((res) => { const s = net.createServer().listen(0, '127.0.0.1', () => { const p = s.address().port; s.close(() => res(p)); }); });
async function record(sc, tmp) {
  const base = path.join(tmp, sc.name), build = path.join(base, sc.build);
  fs.mkdirSync(path.join(build, 'offline'), { recursive: true });
  for (const [f, v] of Object.entries(sc.files || {})) { const p = path.join(build, f); fs.mkdirSync(path.dirname(p), { recursive: true }); fs.writeFileSync(p, v); }
  const cfg = path.join(base, 'config.json');
  fs.writeFileSync(cfg, sc.config);
  fs.rmSync(path.join(OUT, sc.name), { recursive: true, force: true });
  const port = await freePort();
  const child = spawn(process.execPath, [path.join(HERE, 'recorder.mjs'), '--out', OUT, '--name', sc.name, SERVER, '--build', build, '--config', cfg, '--port', String(port), ...(sc.flags || [])], { stdio: ['ignore', 'pipe', 'pipe'] });
  let out = '', err = '';
  child.stdout.on('data', (d) => (out += d)); child.stderr.on('data', (d) => (err += d));
  if (!(await until(() => out.includes('ready 127.0.0.1:'), 10000))) { child.kill('SIGKILL'); throw new Error(`${sc.name}: backend did not start\n${err}`); }
  const c = new Client(port, sc.ua, sc.name), ctx = { expect: [] };
  try { await sc.run(c, ctx); } finally { await sleep(300); c.http.destroy(); child.kill('SIGTERM'); await new Promise((r) => child.once('exit', r)); }
  // a long run of one byte becomes { bodyFill } (keeps the 4 MiB cloud upload out of git)
  const file = path.join(OUT, sc.name, 'session.jsonl');
  const lines = fs.readFileSync(file, 'utf8').split('\n').filter(Boolean).map((l) => {
    const e = JSON.parse(l);
    if (e.req?.body && !e.req.bodyEnc && e.req.body.length > 65536 && /^(.)\1*$/s.test(e.req.body)) { const { body, ...req } = e.req; e.req = { ...req, bodyFill: { length: body.length, char: body[0] } }; }
    return JSON.stringify(e);
  });
  fs.writeFileSync(file, lines.join('\n') + '\n');
  return { ctx, log: c.log };
}

const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'rn-parity-gen-'));
let bad = 0;
try {
  fs.mkdirSync(OUT, { recursive: true });
  for (const sc of SCENARIOS) {
    if (ONLY && !sc.name.includes(ONLY)) continue;
    const t = performance.now();
    const { ctx, log } = await record(sc, tmp);
    const s = readSession(path.join(OUT, sc.name));
    console.log(`${sc.name.padEnd(22)} ${String(s.actions.length).padStart(4)} actions, ${log.length} requests, ${Math.round(performance.now() - t)} ms`);
    if (sc.name === 'routes-17.50') {
      // each route got its canonical and mixed-case request routed to it; each wrong-method request to none
      const covered = new Map(ROUTES.map((r) => [r, new Set()]));
      for (const e of ctx.expect) {
        const got = matchRoute(ROUTES, e.method, e.url);
        if (e.kind === 'wrong-method') { if (got) { bad++; console.log(`  wrong-method request was routed: ${e.method} ${e.url} -> ${got.method} ${got.pattern}`); } continue; }
        if (got !== e.route) { bad++; console.log(`  ${e.kind} ${e.method} ${e.url} hit ${got ? got.method + ' ' + got.pattern : 'no route'} instead of ${e.route.method} ${e.route.pattern}`); continue; }
        covered.get(e.route).add(e.kind);
      }
      const full = [...covered.values()].filter((k) => k.has('canonical') && k.has('mixed-case')).length;
      console.log(`  routes: ${full}/${ROUTES.length} with canonical + mixed-case requests, ${ctx.expect.filter((e) => e.kind === 'wrong-method').length} wrong-method requests`);
      if (full !== ROUTES.length) bad++;
    }
  }
} finally { fs.rmSync(tmp, { recursive: true, force: true }); }
process.exit(bad ? 1 : 0);
