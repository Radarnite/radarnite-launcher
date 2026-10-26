// Progression for the offline backend: play modes, profiles, levels, battle pass, V-bucks and the item shop.
//
// Only active when config.json has "playMode" (the launcher writes it once a mode is picked); without it the backend
// answers exactly as before (everything owned, the locker in offline/state.json, an empty shop).
//
//   playMode "legal"         a profile owns only what it earned or bought: the default outfits and tools, battle pass
//                            rewards, shop purchases. V-bucks are earned in matches and from the battle pass.
//            "legal-vbucks"  the same profile, with V-bucks that never run out (purchases do not spend the earned ones)
//            "unlocked"      the same profile, every cosmetic of the build owned (a view: nothing is written as owned)
//   profile                  name of the save (default "Main"): offline/profiles/<name>.json holds its locker, owned
//                            items, level, XP, battle pass and V-bucks. A new name starts a new profile; "Main" starts
//                            from the locker in offline/state.json, which is left as it is.
//
// Match results: the match server writes one JSON file per finished match into offline/results/ (place, players,
// eliminations, seconds alive; with the game's AI, the bots' difficulty, the bots eliminated and the one that eliminated
// the player, for the bots' ratings); they are applied to the active profile once (by match id) and renamed to *.done.
// Season data (XP per level, battle pass rewards and offer ids) comes from backend/seasons/season<N>.json, generated
// from the build's own Paks by fn-cosmetics-gen --season. The shop is today's real item shop from offline/shop.json
// (radarnite.com's data.json, fetched by the launcher; nothing here goes online), limited to items this build has or
// that were imported into it; without it (or when it is not today's), a daily rotation of the build's own items.
import fs from 'node:fs';
import path from 'node:path';

export const MODES = ['legal', 'legal-vbucks', 'unlocked'];
export const INFINITE_VBUCKS = 999999;

// Rewards of one match. XP: Chapter 2 levels take 80,000 XP; a decent match (10 min, 3 eliminations, top 10) gives
// about half a level. V-bucks: a battle pass (950) after about ten such matches.
export const MATCH_XP = { base: 3000, perKill: 5000, perMinute: 1500, win: 40000, top5: 15000, top10: 10000, top25: 5000 };
export const MATCH_VBUCKS = { base: 25, perKill: 10, win: 150, top5: 50, top10: 25 };
export function matchRewards(r) {
  const kills = Math.max(0, Math.min(99, r.kills | 0)), mins = Math.max(0, Math.min(60, Math.floor((r.seconds | 0) / 60)));
  const place = r.place | 0, won = place === 1;
  const bonus = (t) => won ? t.win : place > 0 && place <= 5 ? t.top5 : place > 0 && place <= 10 ? t.top10 : place > 0 && place <= 25 ? (t.top25 || 0) : 0;
  return { xp: MATCH_XP.base + kills * MATCH_XP.perKill + mins * MATCH_XP.perMinute + bonus(MATCH_XP),
           vbucks: MATCH_VBUCKS.base + kills * MATCH_VBUCKS.perKill + bonus(MATCH_VBUCKS) };
}

// Bot ratings (Elo): each game AI bot, by name and difficulty, has a rating measured by its results against the player,
// and so has the player. A bot the player eliminated lost to the player; the bot that eliminated the player won. A
// new bot starts at its difficulty's rating (difficulty 6, the game's own default skill, starts level with a new player).
export const BOT_ELO = { k: 32, player: 1000, start: (skill) => 400 + 100 * Math.max(1, Math.min(10, skill | 0 || 6)) };
const expected = (a, b) => 1 / (1 + 10 ** ((b - a) / 400));
// r: a match result with botSkill, eliminated [names] and killedBy (name); returns the changes, ratings is updated
export function rateBots(ratings, r) {
  if (!r || !Array.isArray(r.eliminated)) return [];
  ratings.player ??= BOT_ELO.player; ratings.bots ??= {};
  const skill = Math.max(1, Math.min(10, r.botSkill | 0 || 6)), changes = [];
  const game = (rawName, playerWon) => {
    const name = String(rawName || '').trim().slice(0, 64);
    if (!name) return;
    const key = `${skill}:${name}`;
    const b = (ratings.bots[key] ??= { name, skill, rating: BOT_ELO.start(skill), wins: 0, losses: 0 });
    const d = Math.round(BOT_ELO.k * ((playerWon ? 1 : 0) - expected(ratings.player, b.rating)));
    ratings.player += d; b.rating -= d;
    if (playerWon) b.losses++; else b.wins++;
    changes.push({ name, skill, rating: b.rating, delta: -d });
  };
  for (const n of r.eliminated) game(n, true);
  if (r.killedBy) game(r.killedBy, false);
  return changes;
}

// What a new legal profile owns: the build's default outfits (Chapter 1 and 2), pickaxe, glider, contrail, music,
// loading screen and the default dance.
const STARTER = /^(athenacharacter:cid_00[1-8]_athena_commando_[fm]_default|athenacharacter:cid_5(5[6-9]|6[0-3])_athena_commando_[fm]_rebirthdefault[a-d]|athenapickaxe:defaultpickaxe|athenaglider:defaultglider|athenaskydivecontrail:defaultcontrail|athenamusicpack:musicpack_000_default|athenaloadingscreen:lsid_defaultbg|athenadance:eid_dancemoves)$/;
// Shop prices of the fallback rotation, by item type.
const TYPE_PRICE = { AthenaCharacter: 1200, AthenaPickaxe: 800, AthenaGlider: 800, AthenaDance: 500, AthenaBackpack: 400, AthenaItemWrap: 300,
  AthenaSkyDiveContrail: 300, AthenaMusicPack: 200, AthenaLoadingScreen: 200, AthenaPet: 400, AthenaPetCarrier: 400 };

const slug = (name) => String(name || '').trim().toLowerCase().replace(/[^a-z0-9_-]+/g, '-').replace(/^-+|-+$/g, '').slice(0, 32) || 'main';
const today = (t = Date.now()) => new Date(t).toISOString().slice(0, 10);
const nextMidnight = (t = Date.now()) => { const d = new Date(t); d.setUTCHours(24, 0, 0, 0); return d.toISOString(); };
const readJson = (f, d) => { try { return JSON.parse(fs.readFileSync(f, 'utf8').replace(/^﻿/, '')); } catch { return d; } };
// a small deterministic generator (mulberry32) for the daily fallback rotation
function rng(seed) { let a = seed >>> 0; return () => { a = (a + 0x6D2B79F5) >>> 0; let t = a; t = Math.imul(t ^ (t >>> 15), t | 1); t ^= t + Math.imul(t ^ (t >>> 7), t | 61); return ((t ^ (t >>> 14)) >>> 0) / 4294967296; }; }

// config: launcher config.json; cosmetics: { items } of the build (with imported items); off: <build>/offline;
// legacyState: offline/state.json as read (seed of the "Main" profile); dataDir: backend folder (seasons/).
export function createProgress({ config, cosmetics, off, legacyState, dataDir, log = () => {} }) {
  if (!config || typeof config.playMode !== 'string' || !config.playMode) return null;
  const mode = MODES.includes(config.playMode) ? config.playMode : 'legal';
  const profileName = String(config.profile || 'Main').trim().slice(0, 32) || 'Main';
  const dir = path.join(off, 'profiles');
  fs.mkdirSync(dir, { recursive: true });
  const file = path.join(dir, slug(profileName) + '.json');

  let p = readJson(file, null);
  if (!p || typeof p !== 'object') {
    p = { name: profileName, created: new Date().toISOString() };
    // the locker the player had before profiles existed goes to "Main" (copied; state.json stays as it is)
    if (slug(profileName) === 'main' && legacyState && typeof legacyState === 'object')
      for (const k of ['slots', 'favorites', 'seen', 'banner', 'lockerName', 'variantsActive', 'slotVariants']) if (legacyState[k] !== undefined) p[k] = JSON.parse(JSON.stringify(legacyState[k]));
    log(`progress: new profile "${profileName}"`);
  }
  p.slots ??= {}; p.favorites ??= {}; p.seen ??= {}; p.banner ??= { icon: 'BRSeason01', color: 'DefaultColor1' }; p.lockerName ??= ''; p.rvn ??= {};
  p.owned ??= {};           // lower-case templateId -> { templateId, via, at }
  p.vbucks ??= 0;           // earned (and spent) V-bucks; "legal-vbucks" shows INFINITE_VBUCKS on top and never spends them
  p.seasons ??= {};         // season number -> { xp, level, bookPurchased }
  p.results ??= [];         // match ids already applied (the last 200)
  p.stats ??= { matches: 0, wins: 0, kills: 0 };
  p.purchases ??= [];       // { offerId, templateIds, price, at }
  p.botRatings ??= { player: BOT_ELO.player, bots: {} };   // rateBots

  const byId = new Map();
  for (const it of cosmetics.items || []) byId.set(it.templateId.toLowerCase(), it.templateId);
  for (const id of byId.keys()) if (STARTER.test(id) && !p.owned[id]) p.owned[id] = { templateId: byId.get(id), via: 'starter', at: p.created };
  for (const t of ['AthenaPickaxe:defaultpickaxe', 'AthenaGlider:defaultglider', 'AthenaDance:eid_dancemoves', 'AthenaCharacter:cid_001_athena_commando_f_default'])
    if (!p.owned[t.toLowerCase()]) p.owned[t.toLowerCase()] = { templateId: t, via: 'starter', at: p.created };

  const save = () => { try { fs.writeFileSync(file + '.tmp', JSON.stringify(p)); fs.renameSync(file + '.tmp', file); } catch (e) { log('progress: save failed', e.message); } };
  save();

  // ---------- season data ----------
  const seasonCache = new Map();
  function seasonData(n) {
    if (!seasonCache.has(n)) {
      const d = readJson(path.join(dataDir, 'seasons', `season${n}.json`), null);
      if (!d) log(`progress: no seasons/season${n}.json, 80,000 XP per level and no battle pass rewards`);
      seasonCache.set(n, d || { season: n, levelXp: [], free: {}, paid: {}, battlePassOfferIds: [], levelOfferIds: [] });
    }
    return seasonCache.get(n);
  }
  const levelXp = (sd, level) => sd.levelXp?.[level - 1] || 80000;   // XP from level to level + 1
  const maxLevel = (sd) => Math.max(100, sd.levelXp?.length || 0, ...Object.keys(sd.paid || {}).map(Number), ...Object.keys(sd.free || {}).map(Number));
  const season = (n) => (p.seasons[n] ??= { xp: 0, level: 1, bookPurchased: false });

  // ---------- ownership and currency ----------
  const unlocked = mode === 'unlocked', infinite = mode === 'legal-vbucks';
  const owns = (templateId) => unlocked || !!p.owned[String(templateId || '').toLowerCase()];
  const vbucks = () => infinite ? INFINITE_VBUCKS : p.vbucks;
  function grant(templateId, quantity, via, out) {
    const id = String(templateId || '').toLowerCase();
    if (/^currency:mtx/.test(id)) { p.vbucks += quantity | 0; out.vbucks += quantity | 0; return; }
    if (!/^athena/.test(id)) { out.other.push(templateId); return; }   // tokens, XP boosts, banners: nothing to keep here
    if (!p.owned[id]) { p.owned[id] = { templateId: byId.get(id) || templateId, via, at: new Date().toISOString() }; out.items.push(p.owned[id].templateId); }
  }
  const tierRewards = (sd, level, paid) => [...(sd.free?.[level] || []), ...(paid ? sd.paid?.[level] || [] : [])];

  // XP into a season: levels up and hands out the battle pass rewards of each level reached
  function addXp(n, xp, via) {
    const sd = seasonData(n), s = season(n), out = { items: [], vbucks: 0, other: [], levels: 0 };
    s.xp += Math.max(0, xp | 0);
    while (s.level < maxLevel(sd) && s.xp >= levelXp(sd, s.level)) {
      s.xp -= levelXp(sd, s.level); s.level++; out.levels++;
      for (const r of tierRewards(sd, s.level, s.bookPurchased)) grant(r.templateId, r.quantity || 1, via, out);
    }
    return out;
  }

  // ---------- match results (offline/results/*.json from the match server) ----------
  const resultsDir = path.join(off, 'results');
  function applyResult(r, n) {
    if (!r || !r.id || p.results.includes(r.id)) return null;
    const { xp, vbucks: vb } = matchRewards(r);
    p.vbucks += vb;
    const out = addXp(n, xp, 'level');
    p.stats.matches++; p.stats.kills += Math.max(0, r.kills | 0); if ((r.place | 0) === 1) p.stats.wins++;
    p.results.push(r.id); if (p.results.length > 200) p.results.splice(0, p.results.length - 200);
    log(`progress: match ${r.id} place ${r.place}/${r.players} kills ${r.kills} ${r.seconds}s -> +${xp} XP, +${vb + out.vbucks} V-bucks, level ${season(n).level}` +
      (out.items.length ? `, rewards ${out.items.join(' ')}` : ''));
    const rated = rateBots(p.botRatings, r);
    if (rated.length) log(`progress: bot ratings: you ${p.botRatings.player}; ` + rated.map((c) => `${c.name} (difficulty ${c.skill}) ${c.rating} (${c.delta >= 0 ? '+' : ''}${c.delta})`).join(', '));
    return { xp, vbucks: vb, ...out };
  }
  // returns true when anything was applied
  function pollResults(n) {
    let files = [];
    try { files = fs.readdirSync(resultsDir).filter((f) => f.endsWith('.json')); } catch { return false; }
    let any = false;
    for (const f of files.sort()) {
      const full = path.join(resultsDir, f);
      const r = readJson(full, null);
      if (!r) continue;   // still being written: next poll
      if (applyResult(r, r.season || n)) any = true;
      try { fs.renameSync(full, full + '.done'); } catch {}
    }
    if (any) save();
    return any;
  }

  // ---------- item shop ----------
  // imported items by the build asset name the real shop uses ("character_feelunionski" -> AthenaCharacter:cid_fnpt_...)
  // and the build's own items by the name after "Type:" (the shop's ids are bare: "cid_029_athena_commando_f_halloween")
  const byName = new Map();
  for (const it of cosmetics.items || []) {
    if (typeof it.source === 'string' && it.source.startsWith('fn-port:')) byName.set(it.source.split('.').pop().toLowerCase(), it.templateId);
    else byName.set(it.templateId.split(':').pop().toLowerCase(), it.templateId);
  }
  const resolve = (id) => byName.get(String(id || '').toLowerCase()) || null;
  let shopCache = { key: '', offers: [] };
  function realShop() {
    const f = path.join(off, 'shop.json');
    let st; try { st = fs.statSync(f); } catch { return null; }
    const key = `${st.mtimeMs}:${today()}`;
    if (shopCache.key === key) return shopCache.offers;
    const d = readJson(f, null);
    const offers = [];
    if (d && Array.isArray(d.entries) && d.date === today()) {
      const seen = new Set();
      for (const e of d.entries) {
        const grants = (e.items || []).map((i) => resolve(i.id));
        if (!grants.length || grants.some((g) => !g) || !(e.price > 0)) continue;   // a bundle only when the build has all of it
        const offerId = String(e.offer || 'rn:' + grants.join(','));
        if (seen.has(offerId)) continue; seen.add(offerId);
        const kind = grants[0].split(':')[0];
        offers.push({ offerId, devName: `[RN] ${e.title || grants[0]}`, price: e.price, regular: e.regular || e.price, grants,
          section: kind === 'AthenaCharacter' || grants.length > 1 ? 'Featured' : 'Daily' });
      }
    }
    shopCache = { key, offers: offers.length ? offers : null };
    if (d) log(`progress: shop.json of ${d.date}: ${offers.length} offers this build can grant`);
    return shopCache.offers;
  }
  function fallbackShop() {
    const r = rng(Number(today().replace(/-/g, '')));
    const pool = (t) => (cosmetics.items || []).map((i) => i.templateId).filter((x) => x.startsWith(t + ':') && !STARTER.test(x.toLowerCase()) && !/default|_npc_|random/i.test(x));
    const pick = (list, n) => { const out = []; const l = [...list]; while (out.length < n && l.length) out.push(l.splice(Math.floor(r() * l.length), 1)[0]); return out; };
    const offers = [];
    for (const [t, n, section] of [['AthenaCharacter', 4, 'Featured'], ['AthenaPickaxe', 2, 'Daily'], ['AthenaGlider', 1, 'Daily'], ['AthenaDance', 3, 'Daily'], ['AthenaBackpack', 1, 'Daily'], ['AthenaItemWrap', 1, 'Daily']])
      for (const id of pick(pool(t), n)) offers.push({ offerId: 'rn:' + id.toLowerCase(), devName: `[RN] ${id}`, price: TYPE_PRICE[t], regular: TYPE_PRICE[t], grants: [id], section });
    return offers;
  }
  const shopOffers = () => realShop() || fallbackShop();

  const entry = (o, i) => ({
    devName: o.devName, offerId: o.offerId, fulfillmentIds: [], dailyLimit: -1, weeklyLimit: -1, monthlyLimit: -1, categories: [],
    prices: [{ currencyType: 'MtxCurrency', currencySubType: '', regularPrice: o.regular, dynamicRegularPrice: -1, finalPrice: o.price, saleExpiration: '9999-12-31T23:59:59.999Z', basePrice: o.regular }],
    meta: { SectionId: o.section, TileSize: o.section === 'Featured' ? 'Normal' : 'Small' }, matchFilter: '', filterWeight: 0, appStoreId: [],
    requirements: o.grants.map((g) => ({ requirementType: 'DenyOnItemOwnership', requiredId: g, minQuantity: 1 })), offerType: 'StaticPrice',
    giftInfo: { bIsEnabled: false, forcedGiftBoxTemplateId: '', purchaseRequirements: [], giftRecordIds: [] }, refundable: false,
    metaInfo: [{ key: 'SectionId', value: o.section }, { key: 'TileSize', value: o.section === 'Featured' ? 'Normal' : 'Small' }],
    displayAssetPath: '', itemGrants: o.grants.map((g) => ({ templateId: g, quantity: 1 })), sortPriority: -i, catalogGroupPriority: 0 });
  // the battle pass and its levels, with the offer ids the build's season asset names
  function passOffers(n) {
    const sd = seasonData(n), out = [];
    const ids = sd.battlePassOfferIds || [];
    if (ids[0]) out.push({ offerId: ids[0], devName: `BR.Season${n}.BattlePass.01`, price: 950, regular: 950, grants: [], section: 'BattlePass', pass: 'pass' });
    if (ids[1]) out.push({ offerId: ids[1], devName: `BR.Season${n}.BattleBundle.01`, price: 2800, regular: 4700, grants: [], section: 'BattlePass', pass: 'bundle' });
    for (const id of sd.levelOfferIds || []) out.push({ offerId: id, devName: `BR.Season${n}.SingleTier.01`, price: 150, regular: 150, grants: [], section: 'BattlePass', pass: 'level' });
    return out;
  }
  function catalog(n) {
    const storefronts = [];
    if (!unlocked) {
      const offers = shopOffers();
      storefronts.push({ name: 'BRWeeklyStorefront', catalogEntries: offers.filter((o) => o.section === 'Featured').map(entry) });
      storefronts.push({ name: 'BRDailyStorefront', catalogEntries: offers.filter((o) => o.section === 'Daily').map(entry) });
      storefronts.push({ name: `BRSeason${n}`, catalogEntries: passOffers(n).map(entry) });
    }
    return { refreshIntervalHrs: 24, dailyPurchaseHrs: 24, expiration: nextMidnight(), storefronts };
  }
  const shopSections = () => unlocked ? [] : [['Featured', 'Featured', 1], ['Daily', 'Daily', 2]].map(([sectionId, name, landingPriority]) => ({
    bSortOffersByOwnership: false, bShowIneligibleOffersIfGiftable: false, bEnableToastNotification: true, background: { stage: 'default', _type: 'DynamicBackground', key: 'vault' },
    _type: 'ShopSection', landingPriority, bHidden: false, sectionId, bShowTimer: true, sectionDisplayName: name, bShowIneligibleOffers: true }));

  // PurchaseCatalogEntry: { offerId, purchaseQuantity } -> { error } or { lootItems, vbucks }
  function purchase(body, n) {
    const qty = Math.max(1, Math.min(100, body.purchaseQuantity | 0 || 1));
    const offer = [...(unlocked ? [] : shopOffers()), ...passOffers(n)].find((o) => o.offerId === body.offerId);
    if (!offer) return { error: 'errors.com.epicgames.fortnite.catalog_out_of_date', message: `offer ${body.offerId} is not in today's shop` };
    if (unlocked) return { error: 'errors.com.epicgames.fortnite.already_owned', message: 'everything is owned in this mode' };
    const s = season(n), sd = seasonData(n);
    if (offer.pass && offer.pass !== 'level' && s.bookPurchased) return { error: 'errors.com.epicgames.fortnite.already_owned', message: 'battle pass already owned' };
    if (offer.grants.some(owns)) return { error: 'errors.com.epicgames.fortnite.already_owned', message: 'already owned' };
    const price = offer.price * (offer.pass === 'level' ? qty : 1);
    if (!infinite && p.vbucks < price) return { error: 'errors.com.epicgames.modules.gameplayutils.not_enough_mtx', message: `${price} V-bucks needed, ${p.vbucks} left` };
    if (!infinite) p.vbucks -= price;
    const out = { items: [], vbucks: 0, other: [], levels: 0 };
    if (offer.pass === 'pass' || offer.pass === 'bundle') {
      s.bookPurchased = true;
      for (let l = 1; l <= s.level; l++) for (const r of sd.paid?.[l] || []) grant(r.templateId, r.quantity || 1, 'battlepass', out);
      if (offer.pass === 'bundle') for (let i = 0; i < 25; i++) levelUp(n, out);
    } else if (offer.pass === 'level') for (let i = 0; i < qty; i++) levelUp(n, out);
    else for (const g of offer.grants) grant(g, 1, 'shop', out);
    p.purchases.push({ offerId: offer.offerId, templateIds: offer.grants, price: infinite ? 0 : price, at: new Date().toISOString() });
    save();
    log(`progress: bought ${offer.devName} for ${price} V-bucks${infinite ? ' (infinite)' : `, ${p.vbucks} left`}${out.items.length ? ': ' + out.items.join(' ') : ''}`);
    return { lootItems: [...new Set([...offer.grants, ...out.items])], vbucks: vbucks() };
  }
  function levelUp(n, out) {
    const sd = seasonData(n), s = season(n);
    if (s.level >= maxLevel(sd)) return;
    s.level++; s.xp = 0; out.levels++;
    for (const r of tierRewards(sd, s.level, s.bookPurchased)) grant(r.templateId, r.quantity || 1, 'battlepass', out);
  }

  // stats for the athena profile
  const athenaStats = (n) => { const s = season(n); return { level: s.level, book_level: s.level, xp: s.xp, book_xp: 0, book_purchased: s.bookPurchased, accountLevel: s.level,
    season_match_boost: 0, lifetime_wins: p.stats.wins, season: { numWins: p.stats.wins, numHighBracket: 0, numLowBracket: 0 } }; };
  const summary = (n) => ({ mode, profile: p.name, vbucks: vbucks(), earnedVbucks: p.vbucks, owned: Object.keys(p.owned).length, ...season(n), stats: p.stats,
    playerRating: p.botRatings.player, shopDate: readJson(path.join(off, 'shop.json'), {})?.date || null });

  return { mode, unlocked, infinite, profile: p, file, save, owns, vbucks, athenaStats, catalog, shopSections, purchase, pollResults, applyResult, addXp, summary,
    dailyStoreEnd: nextMidnight, seasonData };
}
