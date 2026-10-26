// Progression math of backend/progress.mjs: V-bucks never go negative or get spent twice, a result is applied once,
// the battle pass hands out its rewards, legal profiles own only what they earned. Run by hand: node --test tests/progress/progress.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { createProgress, matchRewards, rateBots, INFINITE_VBUCKS } from '../../backend/progress.mjs';

const cosmetics = { items: [
  { templateId: 'AthenaCharacter:cid_001_athena_commando_f_default' }, { templateId: 'AthenaPickaxe:defaultpickaxe' },
  { templateId: 'AthenaCharacter:cid_a_shop' }, { templateId: 'AthenaCharacter:cid_b_reward' }, { templateId: 'AthenaDance:eid_paid' },
  { templateId: 'AthenaCharacter:cid_fnpt_imported', source: 'fn-port:/BRCosmetics/Athena/Items/Cosmetics/Characters/Character_Imported.Character_Imported' },
] };
const season = { season: 14, levelXp: Array(100).fill(80000), free: { 2: [{ templateId: 'AthenaCharacter:cid_b_reward', quantity: 1 }], 3: [{ templateId: 'Currency:MtxGiveaway', quantity: 100 }] },
  paid: { 1: [{ templateId: 'AthenaDance:eid_paid', quantity: 1 }] }, battlePassOfferIds: ['bp-offer', 'bundle-offer'], levelOfferIds: ['level-offer'] };

function setup(config, shop) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'rn-progress-'));
  const off = path.join(root, 'offline'), data = path.join(root, 'backend');
  fs.mkdirSync(path.join(data, 'seasons'), { recursive: true }); fs.mkdirSync(off, { recursive: true });
  fs.writeFileSync(path.join(data, 'seasons', 'season14.json'), JSON.stringify(season));
  if (shop) fs.writeFileSync(path.join(off, 'shop.json'), JSON.stringify(shop));
  const make = (legacyState = null) => createProgress({ config, cosmetics, off, legacyState, dataDir: data });
  return { root, off, make };
}
const todayShop = () => ({ date: new Date().toISOString().slice(0, 10), entries: [
  { offer: 'v2:/shop', title: 'Shop skin', price: 1200, regular: 1200, items: [{ id: 'cid_a_shop' }] },
  { offer: 'v2:/imp', title: 'Imported', price: 800, regular: 800, items: [{ id: 'character_imported' }] },
  { offer: 'v2:/missing', title: 'Not in build', price: 500, items: [{ id: 'cid_999_not_here' }] },
  { offer: 'v2:/half', title: 'Half bundle', price: 2000, items: [{ id: 'cid_a_shop' }, { id: 'cid_999_not_here' }] },
] });

test('no playMode = no progression (the old behaviour)', () => {
  assert.equal(setup({}).make(), null);
  assert.equal(setup({ playMode: '' }).make(), null);
});

test('legal: buys with earned V-bucks only, once, and owns only what it got', () => {
  const { make } = setup({ playMode: 'legal', profile: 'T' }, todayShop());
  const p = make();
  assert.equal(p.owns('AthenaCharacter:cid_001_athena_commando_f_default'), true);
  assert.equal(p.owns('AthenaCharacter:cid_a_shop'), false);
  const offers = p.catalog(14).storefronts.flatMap((s) => s.catalogEntries.map((e) => e.offerId));
  assert.deepEqual(offers.sort(), ['bp-offer', 'bundle-offer', 'level-offer', 'v2:/imp', 'v2:/shop']);
  assert.match(p.purchase({ offerId: 'v2:/shop' }, 14).error, /not_enough_mtx/);
  p.profile.vbucks = 1500;
  assert.deepEqual(p.purchase({ offerId: 'v2:/shop' }, 14).lootItems, ['AthenaCharacter:cid_a_shop']);
  assert.equal(p.profile.vbucks, 300);
  assert.match(p.purchase({ offerId: 'v2:/shop' }, 14).error, /already_owned/);
  assert.equal(p.profile.vbucks, 300);
  assert.equal(p.owns('AthenaCharacter:cid_a_shop'), true);
  // saved and read back
  const again = make();
  assert.equal(again.profile.vbucks, 300);
  assert.equal(again.owns('AthenaCharacter:cid_a_shop'), true);
});

test('legal-vbucks: infinite balance, earned V-bucks untouched; unlocked: everything owned, nothing written as owned', () => {
  const { off } = setup({}, todayShop());
  const data = path.join(path.dirname(off), 'backend');
  const inf = createProgress({ config: { playMode: 'legal-vbucks', profile: 'T' }, cosmetics, off, dataDir: data });
  assert.equal(inf.vbucks(), INFINITE_VBUCKS);
  assert.ok(inf.purchase({ offerId: 'v2:/imp' }, 14).lootItems);
  assert.equal(inf.profile.vbucks, 0);
  const all = createProgress({ config: { playMode: 'unlocked', profile: 'T' }, cosmetics, off, dataDir: data });
  assert.equal(all.owns('AthenaDance:eid_paid'), true);
  assert.equal(all.profile.owned['athenadance:eid_paid'], undefined);
  const back = createProgress({ config: { playMode: 'legal', profile: 'T' }, cosmetics, off, dataDir: data });
  assert.equal(back.owns('AthenaDance:eid_paid'), false);
  assert.equal(back.owns('AthenaCharacter:cid_fnpt_imported'), true);
});

test('a match result is applied once and levels hand out the battle pass', () => {
  const { off, make } = setup({ playMode: 'legal', profile: 'T' });
  const p = make();
  const r = { id: 'm1', place: 1, players: 20, kills: 10, seconds: 900 };
  const { xp, vbucks } = matchRewards(r);
  assert.equal(xp, 3000 + 50000 + 15 * 1500 + 40000);
  assert.equal(vbucks, 25 + 100 + 150);
  fs.mkdirSync(path.join(off, 'results'));
  fs.writeFileSync(path.join(off, 'results', 'm1.json'), JSON.stringify(r));
  assert.equal(p.pollResults(14), true);
  assert.equal(p.summary(14).level, 2);
  assert.equal(p.owns('AthenaCharacter:cid_b_reward'), true);
  fs.writeFileSync(path.join(off, 'results', 'm1-again.json'), JSON.stringify(r));
  assert.equal(p.pollResults(14), false);
  assert.equal(p.profile.vbucks, vbucks);
  // the pass bought later gives the paid rewards of the levels already reached
  p.profile.vbucks = 1000;
  p.purchase({ offerId: 'bp-offer' }, 14);
  assert.equal(p.profile.vbucks, 50);
  assert.equal(p.owns('AthenaDance:eid_paid'), true);
  // one level bought: level 3 gives 100 V-bucks
  p.profile.vbucks = 150;
  p.purchase({ offerId: 'level-offer', purchaseQuantity: 1 }, 14);
  assert.equal(p.summary(14).level, 3);
  assert.equal(p.profile.vbucks, 100);
});

test('Main starts from the old locker, which stays as it was', () => {
  const { make } = setup({ playMode: 'unlocked' });
  const legacy = { slots: { Character: ['AthenaCharacter:cid_a_shop'] }, favorites: { x: true } };
  const p = make(legacy);
  assert.deepEqual(p.profile.slots, legacy.slots);
  p.profile.slots.Character = ['AthenaCharacter:cid_b_reward'];
  assert.deepEqual(legacy.slots.Character, ['AthenaCharacter:cid_a_shop']);
});

test('without a shop of today: a daily rotation of the build\'s own items', () => {
  const { make } = setup({ playMode: 'legal', profile: 'T' }, { date: '2001-01-01', entries: [] });
  const p = make();
  const ids = p.catalog(14).storefronts.flatMap((s) => s.catalogEntries.map((e) => e.offerId));
  assert.ok(ids.includes('rn:athenacharacter:cid_a_shop') || ids.includes('rn:athenacharacter:cid_b_reward'));
});

test('bot ratings: an elimination moves the player up and the bot down by the same amount, once per result', () => {
  const { make } = setup({ playMode: 'legal', profile: 'R' });
  const pr = make();
  const res = { id: 'm-r1', season: 14, place: 2, players: 7, kills: 1, seconds: 300, botSkill: 6, eliminated: ['Bob'], killedBy: 'Ann' };
  pr.applyResult(res, 14);
  const r = pr.profile.botRatings;
  // equal ratings (1000 vs 1000): +16 for beating Bob, then -16 against Ann (both still near 1000)
  assert.equal(r.bots['6:Bob'].rating, 984);
  assert.equal(r.bots['6:Bob'].losses, 1);
  assert.equal(r.bots['6:Ann'].wins, 1);
  assert.equal(r.player + r.bots['6:Bob'].rating + r.bots['6:Ann'].rating, 3000);   // zero-sum
  pr.applyResult(res, 14);                                                          // the same match again: ignored
  assert.equal(r.bots['6:Bob'].losses, 1);
  // a stronger bot (difficulty 10, 1400) beating the player takes little; a weak one (1, 500) takes a lot
  const strong = rateBots({ player: 1000 }, { eliminated: [], killedBy: 'Pro', botSkill: 10 });
  const weak = rateBots({ player: 1000 }, { eliminated: [], killedBy: 'Noob', botSkill: 1 });
  assert.ok(strong[0].delta > 0 && weak[0].delta > strong[0].delta);
  assert.deepEqual(rateBots({}, { id: 'old result without bots' }), []);
});
