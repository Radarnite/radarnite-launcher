// Fortnite offline backend: a tiny local stand-in for Epic's services, for old Fortnite builds played offline.
//
// - Listens ONLY on 127.0.0.1 (never on the network) and makes NO outgoing connections (no fetch/http client here).
// - One local account; name and id from launcher/config.json. No real Epic account, token or credential is used.
// - Every cosmetic of the running build owned, with every style: <build>/offline/cosmetics.json (generated from the
//   build's own Paks by fn-cosmetics-gen; see the launcher).
// - Locker changes are kept in <build>/offline/state.json.
// Response shapes follow Epic's public API. Unknown routes answer an Epic-style 404 and are logged in <build>/offline/unknown-routes.log.
//
// usage: node server.mjs --build <build root> --config <config.json> [--port 3551]
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';

const arg = (n, d) => { const i = process.argv.indexOf(n); return i > 0 ? process.argv[i + 1] : d; };
const BUILD = arg('--build');
const CONFIG = arg('--config');
if (!BUILD || !CONFIG) { console.error('usage: node server.mjs --build <dir> --config <config.json>'); process.exit(2); }
const OFF = path.join(BUILD, 'offline');
fs.mkdirSync(OFF, { recursive: true });
const readJson = (f, d) => { try { return JSON.parse(fs.readFileSync(f, 'utf8').replace(/^\uFEFF/, '')); } catch { return d; } };

const config = readJson(CONFIG, {});
const PORT = Number(arg('--port', config.port || 3551));
const ACCOUNT_ID = /^[0-9a-f]{32}$/.test(config.accountId || '') ? config.accountId : crypto.randomBytes(16).toString('hex');
const DISPLAY_NAME = String(config.displayName || 'OfflinePlayer').slice(0, 32);
const LOG = path.join(OFF, 'backend.log');
const log = (...a) => { try { fs.appendFileSync(LOG, `${new Date().toISOString()} ${a.join(' ')}\n`); } catch {} };

const cosmetics = readJson(path.join(OFF, 'cosmetics.json'), { items: [] });
// Own cosmetics added on top of the build (fn-modkit newskin, FortnitePorting reports/skin-propia-launcher.md): each is
// an encrypted pak in Paks\ whose key the keychain hands out, plus the item owned here. <build>/offline/custom-cosmetics.json
// = { "items": [{ "templateId", "variants" }], "keychain": ["KEYGUID:base64key:templateId"] }. Kept apart from
// cosmetics.json so regenerating that file does not drop them.
const customCosmetics = readJson(path.join(OFF, 'custom-cosmetics.json'), { items: [], keychain: [] });
cosmetics.items ??= [];
for (const it of customCosmetics.items || [])
  if (it?.templateId && !cosmetics.items.some((c) => c.templateId.toLowerCase() === it.templateId.toLowerCase())) cosmetics.items.push({ variants: [], ...it });
const statePath = path.join(OFF, 'state.json');
const state = readJson(statePath, {});
state.slots ??= {};            // Character, Backpack, Pickaxe, Glider, SkyDiveContrail, MusicPack, LoadingScreen, Dance[6], ItemWrap[7]
state.favorites ??= {};
state.seen ??= {};
state.banner ??= { icon: 'BRSeason01', color: 'DefaultColor1' };
state.lockerName ??= '';
state.rvn ??= {};
const saveState = () => { try { fs.writeFileSync(statePath, JSON.stringify(state)); } catch (e) { log('state save failed', e.message); } };

const now = () => new Date().toISOString();
const FAR = '9999-12-31T23:59:59.999Z';

// ---------- build / season from the client's User-Agent ("Fortnite/++Fortnite+Release-17.50-CL-17388565 Windows/10") ----------
function buildInfo(req) {
  const ua = req.headers['user-agent'] || '';
  const m = ua.match(/Release-(\d+)\.(\d+)(?:\.\d+)?-CL-(\d+)/);
  const major = m ? Number(m[1]) : 0, minor = m ? Number(m[2]) : 0, cl = m ? Number(m[3]) : 0;
  // Season number as the game counts it: 1.x is season 1/2 (by CL), afterwards season == major version.
  let season = major;
  if (major === 1) season = cl <= 3724489 ? 1 : 2; // 1.8+ (CL 3724489) is season 2
  if (major === 2) season = 2;
  return { major, minor, cl, season, version: m ? `${major}.${minor}` : '?' };
}

// ---------- accounts / oauth ----------
// Two local accounts: the player (config.json) and the local game server process (login "server@...").
const SERVER_ID = crypto.createHash('md5').update('server' + ACCOUNT_ID).digest('hex');
const tokens = new Map(); // access token -> account id
function token(login) {
  const isServer = /^server@/i.test(login || '');
  const id = isServer ? SERVER_ID : ACCOUNT_ID, name = isServer ? 'OfflineServer' : DISPLAY_NAME;
  const t = 'eg1~offline-' + crypto.randomBytes(16).toString('hex');
  tokens.set(t, id);
  return { access_token: t, expires_in: 28800, expires_at: FAR, token_type: 'bearer',
    refresh_token: 'offline-refresh-' + crypto.randomBytes(8).toString('hex'), refresh_expires: 86400, refresh_expires_at: FAR,
    account_id: id, client_id: 'ec684b8c687f479fadea3cb2ad83f5c6', internal_client: true, client_service: 'fortnite',
    displayName: name, app: 'fortnite', in_app_id: id, device_id: 'offline-device', product_id: 'prod-fn', application_id: 'fghi4567FNFBKFz3E4TROb0bmPS8h1GW' };
}
const account = () => ({ id: ACCOUNT_ID, displayName: DISPLAY_NAME, name: DISPLAY_NAME, email: `${ACCOUNT_ID}@offline.local`, failedLoginAttempts: 0,
  lastLogin: now(), numberOfDisplayNameChanges: 0, ageGroup: 'UNKNOWN', headless: false, country: 'ES', lastName: 'Offline', preferredLanguage: 'en',
  canUpdateDisplayName: false, tfaEnabled: false, emailVerified: true, minorVerified: false, minorExpected: false, minorStatus: 'NOT_MINOR', cabinedMode: false, hasHashedEmail: false });
const publicAccount = () => ({ id: ACCOUNT_ID, displayName: DISPLAY_NAME, externalAuths: {} });

// ---------- profiles (MCP) ----------
const SLOT_COUNTS = { Character: 1, Backpack: 1, Pickaxe: 1, Glider: 1, SkyDiveContrail: 1, MusicPack: 1, LoadingScreen: 1, Dance: 6, ItemWrap: 7 };
const DEFAULTS = { Character: 'AthenaCharacter:cid_001_athena_commando_f_default', Pickaxe: 'AthenaPickaxe:defaultpickaxe', Glider: 'AthenaGlider:defaultglider', Dance: 'AthenaDance:eid_dancemoves' };
const slotItems = (slot) => { const n = SLOT_COUNTS[slot] || 1; const cur = state.slots[slot] || []; return Array.from({ length: n }, (_, i) => cur[i] ?? (i === 0 && DEFAULTS[slot] ? DEFAULTS[slot] : '')); };
const variantsOf = new Map();
for (const it of cosmetics.items || []) variantsOf.set(it.templateId.toLowerCase(), it.variants || []);
// an item's styles: the importer's list (channel, default option, owned options) with what the player picked on top;
// a saved list never hides channels the importer knows (an old empty entry in state.json is just "no picks")
function stylesOf(id) {
  const out = JSON.parse(JSON.stringify(variantsOf.get(id) || []));
  for (const p of state.variantsActive?.[id] || []) {
    const v = out.find(x => x.channel === p.channel);
    if (v) { v.active = p.active; v.owned = [...new Set([...(v.owned || []), ...(p.owned || []), p.active])]; }
    else out.push(JSON.parse(JSON.stringify(p)));
  }
  return out;
}

function athena(info) {
  const items = {};
  for (const it of cosmetics.items || []) {
    const id = it.templateId.toLowerCase();
    items[id] = { templateId: it.templateId, attributes: { max_level_bonus: 0, level: 1, item_seen: true, xp: 0, variants: stylesOf(id), favorite: !!state.favorites[id], rnd_sel_cnt: 0 }, quantity: 1 };
  }
  for (const id of Object.values(DEFAULTS)) items[id.toLowerCase()] ??= { templateId: id, attributes: { item_seen: true, variants: [], favorite: false }, quantity: 1 };
  const lockerSlots = {};
  for (const slot of Object.keys(SLOT_COUNTS)) lockerSlots[slot] = { items: slotItems(slot), activeVariants: (state.slotVariants?.[slot]) || slotItems(slot).map(() => null) };
  items['sandbox_loadout'] = { templateId: 'CosmeticLocker:cosmeticlocker_athena', attributes: { locker_slots_data: { slots: lockerSlots }, use_count: 0, banner_icon_template: state.banner.icon, banner_color_template: state.banner.color, locker_name: state.lockerName, item_seen: true, favorite: false }, quantity: 1 };
  const s = info.season;
  const fav = (slot, i = 0) => slotItems(slot)[i] || '';
  return {
    _id: ACCOUNT_ID, created: '2018-01-01T00:00:00.000Z', updated: now(), rvn: state.rvn.athena || 1, wipeNumber: 1, accountId: ACCOUNT_ID, profileId: 'athena', version: 'offline', commandRevision: state.rvn.athena || 1,
    items,
    stats: { attributes: {
      use_random_loadout: false, past_seasons: [], season_match_boost: 0, loadouts: ['sandbox_loadout'], mfa_reward_claimed: true, rested_xp_overflow: 0, last_xp_interaction: now(),
      quest_manager: { dailyLoginInterval: now(), dailyQuestRerolls: 1 }, book_level: 100, season_num: s, book_xp: 0, permissions: [], creative_dynamic_xp: {},
      season: { numWins: 0, numHighBracket: 0, numLowBracket: 0 }, vote_data: {}, lifetime_wins: 0, book_purchased: true, rested_xp_exchange: 1, level: 100, rested_xp: 0,
      rested_xp_mult: 1, accountLevel: 500, xp: 0, battlestars: 0, battlestars_season_total: 0, season_friend_match_boost: 0, active_loadout_index: 0, purchased_bp_offers: [],
      purchased_battle_pass_tier_offers: [], last_match_end_datetime: '', mtx_purchase_history_copy: [], last_applied_loadout: 'sandbox_loadout',
      favorite_character: fav('Character'), favorite_backpack: fav('Backpack'), favorite_pickaxe: fav('Pickaxe'), favorite_glider: fav('Glider'),
      favorite_skydivecontrail: fav('SkyDiveContrail'), favorite_musicpack: fav('MusicPack'), favorite_loadingscreen: fav('LoadingScreen'),
      favorite_dance: slotItems('Dance'), favorite_itemwraps: slotItems('ItemWrap'), banner_icon: state.banner.icon, banner_color: state.banner.color,
      favorite_victorypose: '', favorite_consumableemote: '', favorite_callingcard: '', favorite_spray: [], favorite_hat: '', favorite_mapmarker: '',
      party_assist_quest: '', pinned_quest: '', inventory_limit_bonus: 0, daily_rewards: {}, competitive_identity: {}, season_update: 0, alien_style_points: 0,
    } },
  };
}
function commonCore() {
  return { _id: ACCOUNT_ID, created: '2018-01-01T00:00:00.000Z', updated: now(), rvn: state.rvn.common_core || 1, wipeNumber: 1, accountId: ACCOUNT_ID, profileId: 'common_core', version: 'offline', commandRevision: state.rvn.common_core || 1,
    items: { 'Currency:MtxPurchased': { templateId: 'Currency:MtxPurchased', attributes: { platform: 'EpicPC' }, quantity: 0 } },
    stats: { attributes: { survey_data: {}, personal_offers: {}, intro_game_played: true, import_friends_claimed: {}, mtx_purchase_history: { refundsUsed: 0, refundCredits: 3, purchases: [] },
      undo_cooldowns: [], mtx_affiliate_set_time: '', inventory_limit_bonus: 0, current_mtx_platform: 'EpicPC', mtx_affiliate: '', forced_intro_played: 'Coconut',
      weekly_purchases: {}, daily_purchases: {}, ban_history: {}, in_app_purchases: {}, permissions: [], undo_timeout: 'min', monthly_purchases: {},
      allowed_to_send_gifts: false, mfa_enabled: true, allowed_to_receive_gifts: false, gift_history: {} } } };
}
function emptyProfile(profileId) {
  const base = { _id: ACCOUNT_ID, created: '2018-01-01T00:00:00.000Z', updated: now(), rvn: state.rvn[profileId] || 1, wipeNumber: 1, accountId: ACCOUNT_ID, profileId, version: 'offline', commandRevision: state.rvn[profileId] || 1, items: {}, stats: { attributes: {} } };
  if (profileId === 'common_public') base.stats.attributes = { banner_color: state.banner.color, homebase_name: '', banner_icon: state.banner.icon };
  if (profileId === 'creative') base.stats.attributes = { last_used_project: '', max_island_plots: 50, publish_allowed: false, support_code: '', last_used_plot: '', permissions: [], creator_name: '', publish_banned: false, inventory_limit_bonus: 0 };
  return base;
}
function profileFor(id, info) { return id === 'athena' ? athena(info) : id === 'common_core' ? commonCore() : emptyProfile(id); }

function mcp(command, profileId, body, info, rvnClient) {
  let changed = false;
  const slotOf = (cat) => ({ Character: 'Character', Backpack: 'Backpack', Pickaxe: 'Pickaxe', Glider: 'Glider', SkyDiveContrail: 'SkyDiveContrail', MusicPack: 'MusicPack', LoadingScreen: 'LoadingScreen', Dance: 'Dance', ItemWrap: 'ItemWrap', ItemWraps: 'ItemWrap' })[cat];
  const setSlot = (slot, idx, item, variantUpdates) => {
    if (!slot) return;
    const arr = slotItems(slot);
    const all = idx === -1;
    for (let i = 0; i < arr.length; i++) if (all || i === (idx || 0)) arr[i] = item || '';
    state.slots[slot] = arr;
    if (Array.isArray(variantUpdates) && item) {
      const id = item.toLowerCase();
      const cur = stylesOf(id);
      // a channel the item's list does not have yet (own skins and most items start with no variants) is added:
      // before, such updates were dropped and the style went back to the default on the next profile load
      for (const u of variantUpdates) {
        if (!u?.channel || !u.active) continue;
        let v = cur.find(x => x.channel === u.channel);
        if (!v) cur.push(v = { channel: u.channel, active: u.active, owned: [] });
        v.active = u.active;
        v.owned = [...new Set([...(v.owned || []), ...(u.owned || []), u.active])];
      }
      (state.variantsActive ??= {})[id] = cur;
      log(`variants ${item}: ${variantUpdates.map(u => `${u.channel}=${u.active}`).join(', ')}`);
    }
    // the locker slot carries the styles of what it holds (lockerSlots[slot].activeVariants[i].variants)
    const saved = item ? stylesOf(item.toLowerCase()) : null;
    const sv = ((state.slotVariants ??= {})[slot] ??= arr.map(() => null));
    while (sv.length < arr.length) sv.push(null);
    for (let i = 0; i < arr.length; i++) if (all || i === (idx || 0)) sv[i] = saved?.length ? { variants: saved.map(({ channel, active }) => ({ channel, active })) } : null;
    changed = true;
  };
  switch (command) {
    case 'EquipBattleRoyaleCustomization':
      setSlot(slotOf(body.slotName), body.indexWithinSlot ?? 0, body.itemToSlot, body.variantUpdates); break;
    case 'SetCosmeticLockerSlot':
      setSlot(slotOf(body.category), body.slotIndex ?? 0, body.itemToSlot, body.variantUpdates); break;
    case 'SetCosmeticLockerBanner': case 'SetBattleRoyaleBanner':
      state.banner = { icon: body.bannerIconTemplateName || state.banner.icon, color: body.bannerColorTemplateName || state.banner.color }; changed = true; break;
    case 'SetCosmeticLockerName':
      state.lockerName = String(body.name || ''); changed = true; break;
    case 'SetItemFavoriteStatus':
      if (body.targetItemId) { state.favorites[body.targetItemId.toLowerCase()] = !!body.bFavorite; changed = true; } break;
    case 'SetItemFavoriteStatusBatch':
      (body.itemIds || []).forEach((id, i) => { state.favorites[id.toLowerCase()] = !!(body.itemFavStatus || [])[i]; }); changed = true; break;
    case 'PutModularCosmeticLoadout': case 'SetRandomCosmeticLoadoutFlag': case 'MarkItemSeen': case 'SetHardcoreModifier':
    case 'ClientQuestLogin': case 'QueryProfile': case 'SetMtxPlatform': case 'RefreshExpeditions': case 'SetAffiliateName':
    case 'IncrementNamedCounterStat': case 'SetReceiveGiftsEnabled': case 'MarkNewQuestNotificationSent': case 'SetPartyAssistQuest':
    case 'GetMcpTimeForLogin': case 'VerifyRealMoneyPurchase': case 'ClaimMfaEnabled':
    default: break;
  }
  if (changed) { state.rvn[profileId] = (state.rvn[profileId] || 1) + 1; saveState(); }
  const profile = profileFor(profileId, info);
  return { profileRevision: profile.rvn, profileId, profileChangesBaseRevision: profile.rvn, profileChanges: [{ changeType: 'fullProfileUpdate', profile }],
    profileCommandRevision: profile.commandRevision, serverTime: now(), multiUpdate: [], responseVersion: 1 };
}

// ---------- timeline / content ----------
function timeline(info) {
  const s = info.season;
  const events = [`EventFlag.Season${s}`, `EventFlag.LobbySeason${s}`].map(eventType => ({ eventType, activeUntil: FAR, activeSince: '2018-01-01T00:00:00.000Z' }));
  return { channels: {
    'client-matchmaking': { states: [], cacheExpire: FAR },
    'client-events': { states: [{ validFrom: '2018-01-01T00:00:00.000Z', activeEvents: events, state: {
      activeStorefronts: [], eventNamedWeights: {}, seasonNumber: s, seasonTemplateId: `AthenaSeason:athenaseason${s}`, matchXpBonusPoints: 0,
      seasonBegin: '2018-01-01T00:00:00Z', seasonEnd: FAR, seasonDisplayedEnd: FAR, weeklyStoreEnd: FAR, stwEventStoreEnd: FAR, stwWeeklyStoreEnd: FAR,
      sectionStoreEnds: {}, dailyStoreEnd: FAR } }], cacheExpire: FAR },
  }, eventsTimeOffsetHrs: 0, cacheIntervalMins: 10, currentTime: now() };
}
function content(info) {
  const s = info.season;
  const msg = { title: 'Offline', body: `Fortnite ${info.version} offline (local backend). Nothing here talks to Epic.`, spotlight: false, hidden: false, messagetype: 'normal', _type: 'CommonUI Simple Message Base' };
  return { _title: 'Fortnite Game', _activeDate: '2018-01-01T00:00:00.000Z', lastModified: now(), _locale: 'en-US',
    battleroyalenews: { news: { motds: [], messages: [msg] }, _title: 'battleroyalenews', header: '', style: 'None', _noIndex: false, alwaysShow: false, _activeDate: '2018-01-01T00:00:00.000Z', lastModified: now(), _locale: 'en-US' },
    battleroyalenewsv2: { news: { motds: [] }, _title: 'battleroyalenewsv2', _activeDate: '2018-01-01T00:00:00.000Z', lastModified: now(), _locale: 'en-US' },
    emergencynotice: { news: { platform_messages: [], _type: 'Battle Royale News', messages: [] }, _title: 'emergencynotice', _activeDate: '2018-01-01T00:00:00.000Z', lastModified: now(), _locale: 'en-US' },
    emergencynoticev2: { 'jcr:isCheckedOut': true, _title: 'emergencynoticev2', _noIndex: false, emergencynotices: { _type: 'Emergency Notices', emergencynotices: [] }, _activeDate: '2018-01-01T00:00:00.000Z', lastModified: now(), _locale: 'en-US' },
    dynamicbackgrounds: { backgrounds: { backgrounds: [{ stage: `season${s}`, _type: 'DynamicBackground', key: 'lobby' }, { stage: `season${s}`, _type: 'DynamicBackground', key: 'vault' }], _type: 'DynamicBackgroundList' }, _title: 'dynamicbackgrounds', _noIndex: false, _activeDate: '2018-01-01T00:00:00.000Z', lastModified: now(), _locale: 'en-US' },
    shopSections: { sectionList: { _type: 'ShopSectionList', sections: [] }, _title: 'shop-sections', _activeDate: '2018-01-01T00:00:00.000Z', lastModified: now(), _locale: 'en-US' },
    playlistinformation: { conversion_config: { containerName: 'playlist_info', _type: 'Conversion Config', contentName: 'playlists' }, playlist_info: { _type: 'Playlist Information', playlists: [] }, _title: 'playlistinformation', _activeDate: '2018-01-01T00:00:00.000Z', lastModified: now(), _locale: 'en-US' },
    tournamentinformation: { tournament_info: { tournaments: [], _type: 'Tournaments Info' }, _title: 'tournamentinformation', _activeDate: '2018-01-01T00:00:00.000Z', lastModified: now(), _locale: 'en-US' },
    subgameselectdata: { saveTheWorldUnowned: { _type: 'CommonUI Simple Message', message: { image: '', hidden: false, messagetype: 'normal', _type: 'CommonUI Simple Message Base', title: '', body: '', spotlight: false } }, _title: 'subgameselectdata', battleRoyale: { _type: 'CommonUI Simple Message', message: { image: '', hidden: false, messagetype: 'normal', _type: 'CommonUI Simple Message Base', title: '', body: '', spotlight: false } }, creative: { _type: 'CommonUI Simple Message', message: { image: '', hidden: false, messagetype: 'normal', _type: 'CommonUI Simple Message Base', title: '', body: '', spotlight: false } }, saveTheWorld: { _type: 'CommonUI Simple Message', message: { image: '', hidden: false, messagetype: 'normal', _type: 'CommonUI Simple Message Base', title: '', body: '', spotlight: false } }, _activeDate: '2018-01-01T00:00:00.000Z', lastModified: now(), _locale: 'en-US' },
    lobby: { backgroundimage: '', stage: `season${s}`, _title: 'lobby', _activeDate: '2018-01-01T00:00:00.000Z', lastModified: now(), _locale: 'en-US' },
  };
}

// ---------- router ----------
const routes = [];
const on = (method, pattern, fn) => routes.push({ method, re: new RegExp('^' + pattern.replace(/:\w+/g, '([^/]+)').replace(/\*/g, '.*') + '$', 'i'), fn });
const J = (res, code, body) => { const s = JSON.stringify(body); res.writeHead(code, { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(s) }); res.end(s); };
const T = (res, code, text, type = 'text/plain') => { res.writeHead(code, { 'Content-Type': type, 'Content-Length': Buffer.byteLength(text) }); res.end(text); };
const E = (res) => { res.writeHead(204); res.end(); };
const notFound = (res, p) => J(res, 404, { errorCode: 'errors.com.epicgames.common.not_found', errorMessage: `Sorry the resource you were trying to find could not be found (${p})`, messageVars: [p], numericErrorCode: 1004, originatingService: 'any', intent: 'prod', error_description: 'not found', error: 'not_found' });

on('GET', '/offline/health', (q, r) => J(r, 200, { ok: true, accountId: ACCOUNT_ID, displayName: DISPLAY_NAME, cosmetics: (cosmetics.items || []).length, build: path.basename(BUILD), source: cosmetics.source || 'none' }));
// accounts
on('POST', '/account/api/oauth/token', (q, r, m, info, body) => { const f = new URLSearchParams(body.toString()); J(r, 200, token(f.get('username') || '')); });
on('GET', '/account/api/oauth/verify', (q, r) => { const t = (q.headers.authorization || '').replace(/^bearer\s+/i, ''); const id = tokens.get(t) || ACCOUNT_ID; const tk = token(id === SERVER_ID ? 'server@' : ''); J(r, 200, { ...tk, token: t || tk.access_token, account_id: id, session_id: 'offline', auth_method: 'password' }); });
on('DELETE', '/account/api/oauth/sessions/kill*', (q, r) => E(r));
on('GET', '/account/api/oauth/exchange', (q, r) => J(r, 200, { expiresInSeconds: 300, code: 'offline-exchange', creatingClientId: 'ec684b8c687f479fadea3cb2ad83f5c6' }));
on('GET', '/account/api/public/account/displayName/:name', (q, r) => J(r, 200, publicAccount()));
on('GET', '/account/api/public/account/:id/externalAuths', (q, r) => J(r, 200, []));
on('GET', '/account/api/public/account/:id/deviceAuth', (q, r) => J(r, 200, []));
on('GET', '/account/api/public/account/:id', (q, r, m) => J(r, 200, m[1] === SERVER_ID ? { ...account(), id: SERVER_ID, displayName: 'OfflineServer', name: 'OfflineServer' } : account()));
on('GET', '/account/api/public/account', (q, r, m, info, body, url) => J(r, 200, url.searchParams.getAll('accountId').map(id => id === SERVER_ID ? { id, displayName: 'OfflineServer', externalAuths: {} } : publicAccount())));
on('GET', '/account/api/epicdomains/ssodomains', (q, r) => J(r, 200, []));
on('POST', '/fortnite/api/game/v2/tryPlayOnPlatform/account/*', (q, r) => T(r, 200, 'true'));
on('GET', '/fortnite/api/game/v2/enabled_features', (q, r) => J(r, 200, []));
on('POST', '/fortnite/api/game/v2/grant_access/*', (q, r) => E(r));
on('POST', '/fortnite/api/storeaccess/v1/request_access/*', (q, r) => E(r));
on('GET', '/eulatracking/api/shared/agreements/fn*', (q, r) => E(r));
on('GET', '/eulatracking/api/public/agreements/*', (q, r) => E(r));
on('GET', '/socialban/api/public/v1/*', (q, r) => J(r, 200, { bans: [], warnings: [] }));
on('GET', '/waitingroom/api/waitingroom', (q, r) => E(r));
on('GET', '/lightswitch/api/service/bulk/status', (q, r) => J(r, 200, [lightswitch()]));
on('GET', '/lightswitch/api/service/:svc/status', (q, r) => J(r, 200, lightswitch()));
function lightswitch() { return { serviceInstanceId: 'fortnite', status: 'UP', message: 'Fortnite is online', maintenanceUri: null, overrideCatalogIds: ['a7f138b2e51945ffbfdacc1af0541053'], allowedActions: ['PLAY', 'DOWNLOAD'], banned: false, launcherInfoDTO: { appName: 'Fortnite', catalogItemId: '4fe75bbc5a674f4f9b356b5c90567da5', namespace: 'fn' } }; }
// versions
on('GET', '/fortnite/api/version', (q, r) => J(r, 200, { app: 'fortnite', serverDate: now(), overridePropertiesVersion: 'unknown', cln: '0', build: '0', moduleName: 'Fortnite-Core', buildDate: '2021-01-01T00:00:00.000Z', version: 'offline', branch: 'offline', modules: {} }));
on('GET', '/fortnite/api/v2/versioncheck*', (q, r) => J(r, 200, { type: 'NO_UPDATE' }));
on('GET', '/fortnite/api/versioncheck*', (q, r) => J(r, 200, { type: 'NO_UPDATE' }));
// calendar / content / cloud storage
on('GET', '/fortnite/api/calendar/v1/timeline', (q, r, m, info) => J(r, 200, timeline(info)));
on('GET', '/content/api/pages/fortnite-game*', (q, r, m, info) => J(r, 200, content(info)));
// Hotfix files (Epic's "cloudstorage system" files: ini overrides the game applies at login). Used to point the
// game's XMPP (it connects straight to wss://xmpp-service-prod.ol.epicgames.com, not through curl) at this
// backend (presence/party messages stay local).
// [ConsoleVariables]: no content beacon (the client would wait for a beacon on the server's port 15009 that the
// local match server never opens, then cancel matchmaking with FailedToConnectToContentBeacon).
const HOTFIX = {
  'DefaultEngine.ini': [
    '[OnlineSubsystemMcp.Xmpp]', 'bUseSSL=false', `ServerPort=${PORT}`, '',
    '[OnlineSubsystemMcp.Xmpp Prod]', 'ServerAddr="ws://127.0.0.1"', 'Domain=prod.ol.epicgames.com', '',
    '[ConsoleVariables]', 'FortMatchmakingV2.EnableContentBeacon=0', 'FortMatchmakingV2.ContentBeaconFailureCancelsMatchmaking=0',
    'Fort.ShutdownWhenContentBeaconFails=0', '',
  ].join('\r\n'),
};
// Extra hotfix ini files from <build>/offline/hotfix/*.ini (none by default), appended to the file of the same name
// (e.g. DefaultRuntimeOptions.ini with [/Script/FortniteGame.FortRuntimeOptions] bSkipSubgameSelect=true for unattended tests).
try {
  const hd = path.join(OFF, 'hotfix');
  for (const f of fs.existsSync(hd) ? fs.readdirSync(hd).filter((x) => x.toLowerCase().endsWith('.ini')) : [])
    HOTFIX[f] = (HOTFIX[f] ? HOTFIX[f] + '\r\n' : '') + fs.readFileSync(path.join(hd, f), 'utf8').replace(/^\uFEFF/, '');
} catch (e) { log('hotfix dir', e.message); }
const hotfixEntry = (name) => { const b = Buffer.from(HOTFIX[name]); return { uniqueFilename: name, filename: name,
  hash: crypto.createHash('sha1').update(b).digest('hex'), hash256: crypto.createHash('sha256').update(b).digest('hex'),
  length: b.length, contentType: 'application/octet-stream', uploaded: '2021-01-01T00:00:00.000Z', storageType: 'S3', storageIds: {}, doNotCache: true }; };
on('GET', '/fortnite/api/cloudstorage/system', (q, r) => J(r, 200, Object.keys(HOTFIX).map(hotfixEntry)));
on('GET', '/fortnite/api/cloudstorage/system/config', (q, r) => J(r, 200, {}));
on('GET', '/fortnite/api/cloudstorage/system/:file', (q, r, m) => { const n = decodeURIComponent(m[1]); if (!HOTFIX[n]) return notFound(r, n); T(r, 200, HOTFIX[n], 'application/octet-stream'); });
on('GET', '/fortnite/api/cloudstorage/user/:id/:file', (q, r, m) => { const f = path.join(OFF, 'cloud', path.basename(decodeURIComponent(m[2]))); if (fs.existsSync(f)) { const b = fs.readFileSync(f); r.writeHead(200, { 'Content-Type': 'application/octet-stream', 'Content-Length': b.length }); r.end(b); } else E(r); });
on('GET', '/fortnite/api/cloudstorage/user/:id', (q, r) => { const d = path.join(OFF, 'cloud'); const files = fs.existsSync(d) ? fs.readdirSync(d) : []; J(r, 200, files.map(f => { const b = fs.readFileSync(path.join(d, f)); return { uniqueFilename: f, filename: f, hash: crypto.createHash('sha1').update(b).digest('hex'), hash256: crypto.createHash('sha256').update(b).digest('hex'), length: b.length, contentType: 'application/octet-stream', uploaded: now(), storageType: 'S3', storageIds: {}, accountId: ACCOUNT_ID, doNotCache: false }; })); });
on('PUT', '/fortnite/api/cloudstorage/user/:id/:file', (q, r, m, info, body) => { const d = path.join(OFF, 'cloud'); fs.mkdirSync(d, { recursive: true }); if (body.length < 4 * 1024 * 1024) fs.writeFileSync(path.join(d, path.basename(decodeURIComponent(m[2]))), body); E(r); });
// profiles
on('POST', '/fortnite/api/game/v2/profile/:id/:kind/:command', (q, r, m, info, body, url) => {
  let json = {}; try { json = JSON.parse(body.toString() || '{}'); } catch {}
  J(r, 200, mcp(m[3], url.searchParams.get('profileId') || 'athena', json, info, Number(url.searchParams.get('rvn') || -1)));
});
// store / economy (nothing to buy: everything is owned)
on('GET', '/fortnite/api/storefront/v2/catalog', (q, r) => J(r, 200, { refreshIntervalHrs: 24, dailyPurchaseHrs: 24, expiration: FAR, storefronts: [] }));
on('GET', '/fortnite/api/storefront/v2/keychain', (q, r) => J(r, 200, [...readJson(path.join(OFF, 'keychain.json'), []), ...(customCosmetics.keychain || [])]));
on('GET', '/fortnite/api/storefront/v2/gift/check_eligibility/*', (q, r) => J(r, 200, { price: { currencyType: 'MtxCurrency', currencySubType: '', regularPrice: 0, finalPrice: 0, saleExpiration: FAR, basePrice: 0 }, items: [] }));
on('GET', '/fortnite/api/receipts/v1/account/:id/receipts', (q, r) => J(r, 200, []));
on('GET', '/catalog/api/shared/bulk/offers', (q, r) => J(r, 200, {}));
on('GET', '/fortnite/api/game/v2/br-inventory/account/:id', (q, r) => J(r, 200, { stash: { globalcash: 0 } }));
on('GET', '/fortnite/api/game/v2/privacy/account/:id', (q, r) => J(r, 200, { accountId: ACCOUNT_ID, optOutOfPublicLeaderboards: false }));
on('POST', '/fortnite/api/game/v2/privacy/account/:id', (q, r) => J(r, 200, { accountId: ACCOUNT_ID, optOutOfPublicLeaderboards: false }));
on('GET', '/fortnite/api/game/v2/world/info', (q, r) => J(r, 200, {}));
on('GET', '/fortnite/api/game/v2/events/*', (q, r) => J(r, 200, []));
on('GET', '/api/v1/events/Fortnite/download/:id*', (q, r) => J(r, 200, { player: { gameId: 'Fortnite', accountId: ACCOUNT_ID, tokens: [], teams: {}, pendingPayouts: [], pendingPenalties: {}, persistentScores: {}, groupIdentity: {} }, events: [], templates: [], scores: [] }));
on('GET', '/api/v1/events/Fortnite/*', (q, r) => J(r, 200, []));
on('GET', '/fortnite/api/statsv2/account/:id', (q, r) => J(r, 200, { startTime: 0, endTime: 0, stats: {}, accountId: ACCOUNT_ID }));
on('GET', '/statsproxy/api/statsv2/account/:id', (q, r) => J(r, 200, { startTime: 0, endTime: 0, stats: {}, accountId: ACCOUNT_ID }));
on('POST', '/fortnite/api/statsv2/query', (q, r) => J(r, 200, []));
on('POST', '/statsproxy/api/statsv2/query', (q, r) => J(r, 200, []));
on('GET', '/fortnite/api/stats/accountId/:id/bulk/window/alltime', (q, r) => J(r, 200, []));
on('GET', '/fortnite/api/game/v2/leaderboards/*', (q, r) => J(r, 200, { entries: [] }));
on('GET', '/fortnite/api/game/v2/creative/*', (q, r) => J(r, 200, { results: [], hasMore: false }));
on('GET', '/fortnite/api/game/v2/twitch/*', (q, r) => E(r));
on('POST', '/fortnite/api/feedback/*', (q, r) => E(r));
on('POST', '/fortnite/api/game/v2/profileToken/verify/:id', (q, r) => E(r));
on('GET', '/api/v2/interactions/aggregated/Fortnite/:id', (q, r) => J(r, 200, []));
on('GET', '/api/v2/interactions/latest/Fortnite/:id', (q, r) => J(r, 200, []));
on('GET', '/api/v1/Fortnite/get', (q, r) => J(r, 200, {}));
on('POST', '/fortnite/api/game/v2/chat/*', (q, r) => J(r, 200, { GlobalChatRooms: [] }));
// ---------- match server on demand ----------
// The client asks for a playlist when the player presses PLAY (bucketId = buildId:1:region:playlist:...). The backend
// records it as "wanted"; the launcher polls GET /offline/wanted, (re)starts the match server for that playlist and
// posts /offline/server-ready with the playlist it serves. The matchmaker keeps the client queued until a server
// for the wanted playlist became ready after the request (a used match is never reused).
let gameServer = null; // { address, port, playlist, readyAt }
let wanted = { playlist: '', seq: 0, at: 0 };
on('POST', '/offline/server-ready', (q, r, m, info, body) => { try { gameServer = JSON.parse(body.toString()); } catch { gameServer = { port: 7777 }; } gameServer.address = '127.0.0.1'; gameServer.readyAt = Date.now(); log('game server ready', JSON.stringify(gameServer)); E(r); });
on('POST', '/offline/server-down', (q, r) => { if (gameServer) log('game server down'); gameServer = null; E(r); });
on('GET', '/offline/wanted', (q, r) => J(r, 200, { ...wanted, server: gameServer }));
const serverFor = (w) => gameServer && gameServer.playlist === w.playlist && gameServer.readyAt >= w.at ? gameServer : null;
let buildUniqueId = '0';
on('GET', '/fortnite/api/game/v2/matchmakingservice/ticket/player/*', (q, r) => {
  // The session must carry the client's own build id (first field of bucketId), or the client rejects it (FindSessionFailure).
  const bucket = new URL(q.url, 'http://x').searchParams.get('bucketId') || '';
  if (/^\d+$/.test(bucket.split(':')[0])) buildUniqueId = bucket.split(':')[0];
  if (process.argv.includes('--no-match')) return J(r, 403, { errorCode: 'errors.com.epicgames.offline.no_matchmaking', errorMessage: 'Offline: this build has no local match server (lobby only).', numericErrorCode: 1000, originatingService: 'fortnite', intent: 'prod' });
  const pl = (bucket.split(':')[3] || 'playlist_defaultsolo').toLowerCase();
  wanted = { playlist: pl, seq: wanted.seq + 1, at: Date.now() };
  log('matchmaker: wanted', pl, '#' + wanted.seq);
  J(r, 200, { serviceUrl: `ws://127.0.0.1:${PORT}/matchmaking`, ticketType: 'mms-player', payload: 'offline', signature: 'offline' });
});
on('GET', '/fortnite/api/game/v2/matchmaking/account/:id/session/:sid', (q, r, m) => J(r, 200, { accountId: m[1], sessionId: m[2], key: 'offline' }));
on('GET', '/fortnite/api/matchmaking/session/:sid', (q, r, m) => J(r, 200, session(m[1])));
on('POST', '/fortnite/api/matchmaking/session/:sid/join', (q, r) => E(r));
on('POST', '/fortnite/api/matchmaking/session/matchMakingRequest', (q, r) => J(r, 200, []));
on('GET', '/fortnite/api/matchmaking/session/findPlayer/*', (q, r) => E(r));
function session(id) {
  const g = serverFor(wanted) || gameServer || { address: '127.0.0.1', port: 7777 };
  return { id, ownerId: SERVER_ID, ownerName: 'OfflineServer', serverName: 'OfflineServer', serverAddress: g.address, serverPort: g.port, maxPublicPlayers: 220, openPublicPlayers: 175,
    maxPrivatePlayers: 0, openPrivatePlayers: 0, attributes: { REGION_s: 'EU', GAMEMODE_s: 'FORTATHENA', ALLOWBROADCASTING_b: true, SUBREGION_s: 'GB', DCID_s: 'FORTNITE-LIVEEUGCEC1C2E30UBRCORE0A-14840880', tenant_s: 'Fortnite', MATCHMAKINGPOOL_s: 'Any', STORMSHIELDDEFENSETYPE_i: 0, HOTFIXVERSION_i: 0, PLAYLISTNAME_s: wanted.playlist || 'playlist_defaultsolo', SESSIONKEY_s: 'offline', TENANT_s: 'Fortnite', BEACONPORT_i: 15009 },
    publicPlayers: [], privatePlayers: [], totalPlayers: 0, allowJoinInProgress: false, shouldAdvertise: false, isDedicated: false, usesStats: false, allowInvites: false, usesPresence: false,
    allowJoinViaPresence: true, allowJoinViaPresenceFriendsOnly: false, buildUniqueId, lastUpdated: now(), started: false };
}
on('GET', '/fortnite/api/discovery/accessToken/*', (q, r) => J(r, 200, { branchName: 'offline', appId: 'Fortnite', token: 'offline' }));
// ---------- modes in the game's own menu (lobby "CHANGE": the Discovery surface) ----------
// The lobby lists "links" (mnemonic = playlist name in lower case). The client shows each playlist's own name and
// description from the build (FortPlaylistAthena UIDisplayName/UIDescription); the tile image is a URL, served here
// from offline\ui\ (textures exported from the build's Paks by the cosmetics generator, if present).
// Practice = the Creative "Playground" playlist; the launcher decides what the match server runs for each mnemonic.
const MODE_LINKS = [
  { code: 'playlist_playground', img: 'cm' },
  { code: 'playlist_defaultsolo', img: 'br' },
  { code: 'playlist_defaultduo', img: 'br' },
  { code: 'playlist_trios', img: 'br' },
  { code: 'playlist_defaultsquad', img: 'br' },
];
const linkData = (l) => ({ namespace: 'fn', mnemonic: l.code, linkType: 'BR:Playlist', active: true, disabled: false, version: 1, moderationStatus: 'Unmoderated',
  accountId: 'epic', creatorName: 'Epic', descriptionTags: [], metadata: { image_url: `https://cdn2.unrealengine.com/offline/ui/${l.img}.jpg`, matchmaking: { override_playlist: l.code } } });
const surfaceAsset = () => ({ FortCreativeDiscoverySurface: { meta: { promotion: 1 }, assets: { CreativeDiscoverySurface_Frontend: {
  meta: { revision: 1, headRevision: 1, revisedAt: '2021-01-01T00:00:00.000Z', promotion: 1, promotedAt: '2021-01-01T00:00:00.000Z' },
  assetData: { AnalyticsId: 'offline', SurfaceName: 'CreativeDiscoverySurface_Frontend', TestName: 'offline', primaryAssetId: 'FortCreativeDiscoverySurface:CreativeDiscoverySurface_Frontend',
    GlobalLinkCodeBlacklist: [], GlobalLinkCodeWhitelist: [],
    TestCohorts: [{ AnalyticsId: 'offline', CohortSelector: 'PlayerDeterministic', PlatformBlacklist: [], PlatformWhitelist: [], SelectionChance: 1, TestName: 'offline',
      ContentPanels: [{ NumPages: 1, AnalyticsId: 'offline', PanelType: 'AnalyticsList', AnalyticsListName: 'ByEpicWoven', CuratedListOfLinkCodes: [], ModelName: '', PageSize: MODE_LINKS.length,
        PlatformBlacklist: [], PlatformWhitelist: [], PanelName: 'ByEpicWoven', MetricInterval: '', SkippedEntriesCount: 0, SkippedEntriesPercent: 0, SplicedEntries: [],
        EntrySkippingMethod: 'None', PlayHistoryType: 'RecentlyPlayed', bLowestToHighest: false, PanelLinkCodeBlacklist: [], PanelLinkCodeWhitelist: [], FeatureTags: [], MetricName: '',
        PanelDisplayName: { Category: 'Game', NativeCulture: '', Namespace: 'CreativeDiscoverySurface_Frontend', LocalizedStrings: [], bIsMinimalPatch: false, NativeString: 'Offline', Key: 'ByEpicWoven' } }] }] } } } } });
const surface = () => ({ Panels: [{ PanelName: 'ByEpicWoven', Pages: [{ results: MODE_LINKS.map(l => ({ linkData: linkData(l), lastVisited: null, linkCode: l.code, isFavorite: false })), hasMore: false }] }],
  TestCohorts: ['offline'], ModeSets: {} });
on('POST', '/fortnite/api/game/v2/creative/discovery/surface/*', (q, r) => J(r, 200, surface()));
on('POST', '/discovery/surface/*', (q, r) => J(r, 200, surface()));
on('POST', '/api/v1/assets/Fortnite/*', (q, r) => J(r, 200, surfaceAsset()));
on('POST', '/links/api/fn/mnemonic', (q, r) => J(r, 200, MODE_LINKS.map(linkData)));
on('GET', '/links/api/fn/mnemonic/:code/related', (q, r, m) => { const l = MODE_LINKS.find(x => x.code === m[1].toLowerCase()); J(r, 200, { parentLinks: [], links: l ? { [l.code]: linkData(l) } : {} }); });
on('GET', '/links/api/fn/mnemonic/:code', (q, r, m) => { const l = MODE_LINKS.find(x => x.code === m[1].toLowerCase()); l ? J(r, 200, linkData(l)) : notFound(r, m[1]); });
on('POST', '/api/v1/links/lock-status/:id/check', (q, r, m, info, body) => { let j = {}; try { j = JSON.parse(body.toString() || '{}'); } catch {}
  J(r, 200, { results: (j.linkCodes || []).map(c => ({ playerId: m[1], linkCode: c, lockStatus: 'UNLOCKED', lockStatusReason: 'NONE', isVisible: true })), hasMore: false }); });
on('GET', '/offline/ui/:file', (q, r, m) => { const f = path.join(OFF, 'ui', path.basename(m[1])); if (!fs.existsSync(f)) return notFound(r, m[1]); const b = fs.readFileSync(f);
  r.writeHead(200, { 'Content-Type': f.endsWith('.png') ? 'image/png' : 'image/jpeg', 'Content-Length': b.length }); r.end(b); });
// friends / party / presence (no XMPP: solo lobby)
on('GET', '/friends/api/public/friends/:id', (q, r) => J(r, 200, []));
on('GET', '/friends/api/v1/:id/summary', (q, r) => J(r, 200, { friends: [], incoming: [], outgoing: [], suggested: [], blocklist: [], settings: { acceptInvites: 'public' } }));
on('GET', '/friends/api/v1/:id/friends', (q, r) => J(r, 200, []));
on('GET', '/friends/api/v1/:id/blocklist', (q, r) => J(r, 200, []));
on('GET', '/friends/api/v1/:id/settings', (q, r) => J(r, 200, { acceptInvites: 'public' }));
on('GET', '/friends/api/public/blocklist/:id', (q, r) => J(r, 200, { blockedUsers: [] }));
on('GET', '/friends/api/public/list/fortnite/:id/recentPlayers', (q, r) => J(r, 200, []));
on('GET', '/friends/api/v1/:id/recent/fortnite', (q, r) => J(r, 200, []));
on('GET', '/party/api/v1/Fortnite/user/:id', (q, r) => J(r, 200, { current: [], pending: [], invites: [], pings: [] }));
on('GET', '/party/api/v1/Fortnite/user/:id/notifications/undelivered/count', (q, r) => J(r, 200, { pings: 0, invites: 0 }));
on('POST', '/party/api/v1/Fortnite/parties', (q, r, m, info, body) => {
  let j = {}; try { j = JSON.parse(body.toString() || '{}'); } catch {}
  const id = crypto.randomBytes(16).toString('hex');
  J(r, 200, { id, created_at: now(), updated_at: now(), config: { ...(j.config || {}), type: 'DEFAULT', joinability: 'OPEN', discoverability: 'ALL', sub_type: 'default', max_size: 16, invite_ttl: 14400, join_confirmation: false },
    members: [{ account_id: ACCOUNT_ID, meta: j.join_info?.meta || {}, connections: [{ id: j.join_info?.connection?.id || '', connected_at: now(), updated_at: now(), yield_leadership: false, meta: j.join_info?.connection?.meta || {} }], revision: 0, updated_at: now(), joined_at: now(), role: 'CAPTAIN' }],
    applicants: [], meta: j.meta || {}, invites: [], revision: 0, intentions: [] });
});
on('PATCH', '/party/api/v1/Fortnite/parties/*', (q, r) => E(r));
on('DELETE', '/party/api/v1/Fortnite/parties/*', (q, r) => E(r));
on('POST', '/party/api/v1/Fortnite/parties/*', (q, r) => E(r));
on('GET', '/presence/api/v1/_/:id/settings/subscriptions', (q, r) => J(r, 200, []));
on('GET', '/presence/api/v1/:ns/:id/last-online', (q, r) => J(r, 200, {}));
on('GET', '/presence/api/v1/*', (q, r) => J(r, 200, []));
on('POST', '/presence/api/v1/*', (q, r) => E(r));
// telemetry and misc: accepted and dropped locally
on('POST', '/datarouter/api/v1/public/data*', (q, r) => E(r));
on('POST', '/telemetry/*', (q, r) => E(r));
on('GET', '/region', (q, r) => J(r, 200, { continent: { code: 'EU', geoname_id: 6255148, names: { en: 'Europe' } }, country: { iso_code: 'ES', names: { en: 'Spain' } }, subdivisions: [] }));
on('GET', '/content-controls/*', (q, r) => J(r, 200, {}));
on('POST', '/content-controls/*', (q, r) => J(r, 200, {}));
on('GET', '/hotconfigs/v2/*', (q, r) => J(r, 200, {}));
on('GET', '/api/v1/user/setting', (q, r) => J(r, 200, []));
on('POST', '/api/v1/user/setting', (q, r) => J(r, 200, []));
on('GET', '/fortnite/api/game/v2/homebase/*', (q, r) => J(r, 200, {}));
on('GET', '/affiliate/api/public/affiliates/slug/*', (q, r) => notFound(r, 'affiliate'));

const unknown = new Set();
const server = http.createServer((req, res) => {
  const url = new URL(req.url, 'http://127.0.0.1');
  const chunks = [];
  req.on('data', c => { if (chunks.length < 4096) chunks.push(c); });
  req.on('end', () => {
    const body = Buffer.concat(chunks);
    const info = buildInfo(req);
    for (const r of routes) {
      if (r.method !== req.method) continue;
      const m = url.pathname.match(r.re);
      if (!m) continue;
      try { return r.fn(req, res, m, info, body, url); } catch (e) { log('ERR', req.method, url.pathname, e.stack); return J(res, 500, { errorCode: 'errors.com.epicgames.offline.internal', errorMessage: e.message }); }
    }
    const key = `${req.method} ${url.pathname}`;
    if (!unknown.has(key)) { unknown.add(key); try { fs.appendFileSync(path.join(OFF, 'unknown-routes.log'), `${now()} ${info.version} ${key}\n`); } catch {} }
    notFound(res, url.pathname);
  });
});
// ---------- minimal WebSocket (RFC 6455) ----------
function wsAccept(req, socket, protocol) {
  const accept = crypto.createHash('sha1').update(req.headers['sec-websocket-key'] + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
  socket.write(`HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ${accept}\r\n` + (protocol ? `Sec-WebSocket-Protocol: ${protocol}\r\n` : '') + '\r\n');
  const ws = { onText: () => {}, send(text) { if (socket.destroyed) return; const p = Buffer.from(text); let h;
      if (p.length < 126) h = Buffer.from([0x81, p.length]); else if (p.length < 65536) h = Buffer.from([0x81, 126, p.length >> 8, p.length & 255]);
      else { h = Buffer.alloc(10); h[0] = 0x81; h[1] = 127; h.writeBigUInt64BE(BigInt(p.length), 2); }
      socket.write(Buffer.concat([h, p])); },
    // code 1000 = normal closure (status in the close frame); without a code the peer sees 1005
    close(code) { try { socket.end(code ? Buffer.from([0x88, 2, code >> 8, code & 255]) : Buffer.from([0x88, 0])); } catch {} } };
  let buf = Buffer.alloc(0);
  socket.on('error', () => {});
  socket.on('data', (d) => {
    buf = Buffer.concat([buf, d]);
    while (buf.length >= 2) {
      const op = buf[0] & 15, masked = buf[1] & 128; let len = buf[1] & 127, off = 2;
      if (len === 126) { if (buf.length < 4) return; len = buf.readUInt16BE(2); off = 4; }
      else if (len === 127) { if (buf.length < 10) return; len = Number(buf.readBigUInt64BE(2)); off = 10; }
      const mask = masked ? buf.subarray(off, off + 4) : null; if (masked) off += 4;
      if (buf.length < off + len) return;
      const payload = Buffer.from(buf.subarray(off, off + len)); buf = buf.subarray(off + len);
      if (mask) for (let i = 0; i < payload.length; i++) payload[i] ^= mask[i & 3];
      if (op === 1) ws.onText(payload.toString());
      else if (op === 8) { ws.close(); socket.destroy(); }
      else if (op === 9) socket.write(Buffer.concat([Buffer.from([0x8A, payload.length]), payload]));
    }
  });
  return ws;
}

// ---------- minimal XMPP over WebSocket (RFC 7395), enough for the client to log in, bind and send presence ----------
// The game only reaches it if it was told to use ws://127.0.0.1:<port>/xmpp (launcher passes an -ini override);
// otherwise it tries Epic's wss server, which the firewall blocks.
function xmpp(ws) {
  const domain = 'prod.ol.epicgames.com', id = () => crypto.randomBytes(8).toString('hex');
  let jid = '', authed = false;
  const attr = (x, n) => (x.match(new RegExp(`${n}=["']([^"']*)["']`)) || [])[1] || '';
  ws.onText = (x) => {
    try {
      if (x.startsWith('<open')) {
        ws.send(`<open xmlns="urn:ietf:params:xml:ns:xmpp-framing" from="${domain}" id="${id()}" version="1.0" xml:lang="en"/>`);
        ws.send(authed
          ? `<stream:features xmlns:stream="http://etherx.jabber.org/streams"><bind xmlns="urn:ietf:params:xml:ns:xmpp-bind"/><session xmlns="urn:ietf:params:xml:ns:xmpp-session"><optional/></session></stream:features>`
          : `<stream:features xmlns:stream="http://etherx.jabber.org/streams"><mechanisms xmlns="urn:ietf:params:xml:ns:xmpp-sasl"><mechanism>PLAIN</mechanism></mechanisms><ver xmlns="urn:xmpp:features:rosterver"/></stream:features>`);
      } else if (x.startsWith('<auth')) {
        const plain = Buffer.from((x.match(/>([^<]+)</) || [])[1] || '', 'base64').toString().split('\0');
        const who = (plain[1] || '').split('@')[0] || ACCOUNT_ID;
        jid = `${/^[0-9a-f]{32}$/.test(who) ? who : ACCOUNT_ID}@${domain}`;
        authed = true; ws.send('<success xmlns="urn:ietf:params:xml:ns:xmpp-sasl"/>');
      } else if (x.startsWith('<iq')) {
        const iqId = attr(x, 'id');
        if (x.includes('urn:ietf:params:xml:ns:xmpp-bind')) {
          const res = (x.match(/<resource>([^<]*)<\/resource>/) || [])[1] || 'V2:Fortnite:WIN';
          jid = `${jid.split('/')[0]}/${res}`;
          ws.send(`<iq type="result" id="${iqId}" to="${jid}" xmlns="jabber:client"><bind xmlns="urn:ietf:params:xml:ns:xmpp-bind"><jid>${jid}</jid></bind></iq>`);
        } else ws.send(`<iq type="result" id="${iqId}" to="${jid}" from="${domain}" xmlns="jabber:client"/>`);
      } else if (x.startsWith('<presence')) {
        ws.send(x.replace('<presence', `<presence from="${jid}" to="${jid.split('/')[0]}"`).replace(/ to="[^"]*"( to=)/, '$1'));
      } else if (x.startsWith('<close')) { ws.send('<close xmlns="urn:ietf:params:xml:ns:xmpp-framing"/>'); ws.close(); }
      // <message/> and anything else: accepted and dropped (no other players here)
    } catch (e) { log('xmpp error', e.message); }
  };
}

server.on('upgrade', (req, socket) => {
  if (req.url.startsWith('/xmpp') || (req.headers['sec-websocket-protocol'] || '').includes('xmpp')) {
    xmpp(wsAccept(req, socket, (req.headers['sec-websocket-protocol'] || '').includes('xmpp') ? 'xmpp' : null));
    log('xmpp: client connected');
    return;
  }
  if (!req.url.startsWith('/matchmaking')) return socket.destroy();
  const ws = wsAccept(req, socket, null);
  const send = (obj) => ws.send(JSON.stringify(obj));
  const ticketId = crypto.randomBytes(16).toString('hex'), matchId = crypto.randomBytes(16).toString('hex'), sessionId = crypto.randomBytes(16).toString('hex');
  const w = wanted, t0 = Date.now();
  const steps = [
    () => send({ payload: { state: 'Connecting' }, name: 'StatusUpdate' }),
    () => send({ payload: { totalPlayers: 1, connectedPlayers: 1, state: 'Waiting' }, name: 'StatusUpdate' }),
  ];
  steps.forEach((f, i) => setTimeout(f, 200 + i * 400));
  // Queued until the match server for this playlist is ready (starting one takes 30-120 s), then the assignment.
  let closed = false; socket.on('close', () => { closed = true; }); socket.on('error', () => { closed = true; });
  const poll = setInterval(() => {
    if (closed) return clearInterval(poll);
    const g = serverFor(w);
    if (!g) return send({ payload: { ticketId, queuedPlayers: 0, estimatedWaitSec: Math.max(5, 90 - Math.round((Date.now() - t0) / 1000)), status: {}, state: 'Queued' }, name: 'StatusUpdate' });
    clearInterval(poll);
    send({ payload: { matchId, state: 'SessionAssignment' }, name: 'StatusUpdate' });
    // 17.x closes the socket itself right after "Play"; 3.x waits for the service to close it (it sat ~20 s on
    // "Finding match" with the server already up), so the service closes it like Epic's does: 1000, normal closure.
    setTimeout(() => { if (!closed) send({ payload: { matchId, sessionId, joinDelaySec: 1 }, name: 'Play' }); }, 400);
    setTimeout(() => { if (!closed) ws.close(1000); }, 900);
    log('matchmaker: client sent to', JSON.stringify(g));
  }, 1000);
});
server.listen(PORT, '127.0.0.1', () => { log(`listening on 127.0.0.1:${PORT} as ${DISPLAY_NAME} (${ACCOUNT_ID}), ${(cosmetics.items || []).length} cosmetics`); console.log(`ready 127.0.0.1:${PORT}`); });
server.on('error', e => { log('server error', e.message); console.error(e.message); process.exit(1); });
