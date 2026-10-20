//! Profiles (MCP) and locker state: server.mjs lines 84-201. The state object is the parsed state.json; it is
//! mutated in the same order as the JS so a request that throws half way leaves the same in-memory state.

use crate::app::{App, Info, SINCE};
use crate::js::{self, type_error, JsResult, Obj};
use serde_json::{json, Value};

pub const SLOT_COUNTS: [(&str, usize); 9] = [
    ("Character", 1),
    ("Backpack", 1),
    ("Pickaxe", 1),
    ("Glider", 1),
    ("SkyDiveContrail", 1),
    ("MusicPack", 1),
    ("LoadingScreen", 1),
    ("Dance", 6),
    ("ItemWrap", 7),
];
pub const DEFAULTS: [(&str, &str); 4] = [
    ("Character", "AthenaCharacter:cid_001_athena_commando_f_default"),
    ("Pickaxe", "AthenaPickaxe:defaultpickaxe"),
    ("Glider", "AthenaGlider:defaultglider"),
    ("Dance", "AthenaDance:eid_dancemoves"),
];

fn slot_count(slot: &str) -> usize {
    SLOT_COUNTS.iter().find(|s| s.0 == slot).map_or(1, |s| s.1)
}

fn default_of(slot: &str) -> Option<&'static str> {
    DEFAULTS.iter().find(|d| d.0 == slot).map(|d| d.1)
}

/// slotItems(slot): the slot's items, defaults where empty
pub fn slot_items(st: &Obj, slot: &str) -> Vec<Value> {
    let n = slot_count(slot);
    let cur = st.get("slots").and_then(|s| js::get(s, slot));
    let cur = if js::truthy(cur.as_ref()) { cur } else { None };
    (0..n)
        .map(|i| match cur.as_ref().and_then(|c| js::get(c, &i.to_string())) {
            Some(v) if !v.is_null() => v,
            _ => Value::from(if i == 0 { default_of(slot).unwrap_or("") } else { "" }),
        })
        .collect()
}

/// state.variantsActive?.[id]
fn picks_of(st: &Obj, id: &str) -> Option<Value> {
    js::opt_chain(st.get("variantsActive"), id)
}

/// stylesOf(id): the importer's list (channel, default option, owned options) with what the player picked on top;
/// a saved list never hides channels the importer knows
pub fn styles_of(app: &App, st: &Obj, id: &str) -> JsResult<Value> {
    let mut out = js::or(app.variants_of.get(id).cloned(), json!([]));
    let picks = picks_of(st, id);
    if !js::truthy(picks.as_ref()) {
        return Ok(out);
    }
    for p in js::spread(picks.as_ref(), "state.variantsActive?.[id]")? {
        let Value::Array(arr) = &mut out else {
            return Err(type_error("out.find is not a function"));
        };
        let mut found = None;
        for (k, x) in arr.iter().enumerate() {
            let xc = js::prop(x, "channel")?;
            let pc = js::prop(&p, "channel")?;
            if js::strict_eq(xc.as_ref(), pc.as_ref()) {
                found = Some(k);
                break;
            }
        }
        match found {
            Some(k) => {
                let pa = js::get(&p, "active");
                let v = &mut arr[k];
                js::put(v, "active", pa.clone())?;
                let mut all: Vec<Option<Value>> = Vec::new();
                let vo = js::get(v, "owned");
                if js::truthy(vo.as_ref()) {
                    all.extend(js::spread(vo.as_ref(), "v.owned")?.into_iter().map(Some));
                }
                let po = js::get(&p, "owned");
                if js::truthy(po.as_ref()) {
                    all.extend(js::spread(po.as_ref(), "p.owned")?.into_iter().map(Some));
                }
                all.push(pa);
                js::put(v, "owned", Some(Value::Array(js::dedupe(all))))?;
            }
            None => arr.push(p.clone()),
        }
    }
    Ok(out)
}

/// ({ Character: 'Character', ..., ItemWrap: 'ItemWrap', ItemWraps: 'ItemWrap' })[cat]
fn slot_of(cat: Option<&Value>) -> Option<&'static str> {
    let k = js::to_string(cat);
    match k.as_str() {
        "ItemWraps" => Some("ItemWrap"),
        _ => SLOT_COUNTS.iter().find(|s| s.0 == k).map(|s| s.0),
    }
}

fn ensure_obj(st: &mut Obj, k: &str) {
    if st.get(k).map_or(true, Value::is_null) {
        js::set(st, k, json!({}));
    }
}

fn slot_variant(saved: &Value) -> JsResult<Value> {
    let Value::Array(a) = saved else {
        return Err(type_error("saved.map is not a function"));
    };
    let mut vars = Vec::new();
    for x in a {
        if x.is_null() {
            return Err(type_error("Cannot destructure property 'channel' of 'object null' as it is null."));
        }
        let mut o = Obj::new();
        js::put_opt(&mut o, "channel", js::get(x, "channel"));
        js::put_opt(&mut o, "active", js::get(x, "active"));
        vars.push(Value::Object(o));
    }
    Ok(json!({ "variants": vars }))
}

/// setSlot(slot, idx, item, variantUpdates)
fn set_slot(
    app: &App,
    st: &mut Obj,
    slot: Option<&'static str>,
    idx: Value,
    item: Option<Value>,
    variant_updates: Option<Value>,
    changed: &mut bool,
) -> JsResult<()> {
    let Some(slot) = slot else { return Ok(()) };
    let mut arr = slot_items(st, slot);
    let all = matches!(&idx, Value::Number(n) if n.as_f64() == Some(-1.0));
    // `i === (idx || 0)`
    let sel = if js::truthy(Some(&idx)) {
        match &idx {
            Value::Number(n) => n.as_f64(),
            _ => None,
        }
    } else {
        Some(0.0)
    };
    let hit = |i: usize| all || sel == Some(i as f64);
    let item_val = js::or(item.clone(), json!(""));
    for (i, v) in arr.iter_mut().enumerate() {
        if hit(i) {
            *v = item_val.clone();
        }
    }
    match st.get_mut("slots") {
        Some(s) => js::put(s, slot, Some(Value::Array(arr.clone())))?,
        None => return Err(type_error(format!("Cannot set properties of undefined (setting '{}')", slot))),
    }
    let item_truthy = js::truthy(item.as_ref());
    if let (Some(Value::Array(updates)), true) = (&variant_updates, item_truthy) {
        let id = js::lower(item.as_ref(), "item")?;
        let mut cur = styles_of(app, st, &id)?;
        // a channel the item's list does not have yet (own skins and most items start with no variants) is added
        for u in updates {
            let uc = js::opt_chain(Some(u), "channel");
            if !js::truthy(uc.as_ref()) {
                continue;
            }
            let ua = js::get(u, "active");
            if !js::truthy(ua.as_ref()) {
                continue;
            }
            let Value::Array(list) = &mut cur else {
                return Err(type_error("cur.find is not a function"));
            };
            let mut k = None;
            for (i, x) in list.iter().enumerate() {
                if js::strict_eq(js::prop(x, "channel")?.as_ref(), uc.as_ref()) {
                    k = Some(i);
                    break;
                }
            }
            let k = match k {
                Some(k) => k,
                None => {
                    let mut o = Obj::new();
                    js::put_opt(&mut o, "channel", uc.clone());
                    js::put_opt(&mut o, "active", ua.clone());
                    o.insert("owned".into(), json!([]));
                    list.push(Value::Object(o));
                    list.len() - 1
                }
            };
            let v = &mut list[k];
            js::put(v, "active", ua.clone())?;
            let mut owned: Vec<Option<Value>> = Vec::new();
            let vo = js::get(v, "owned");
            if js::truthy(vo.as_ref()) {
                owned.extend(js::spread(vo.as_ref(), "v.owned")?.into_iter().map(Some));
            }
            let uo = js::get(u, "owned");
            if js::truthy(uo.as_ref()) {
                owned.extend(js::spread(uo.as_ref(), "u.owned")?.into_iter().map(Some));
            }
            owned.push(ua);
            js::put(v, "owned", Some(Value::Array(js::dedupe(owned))))?;
        }
        ensure_obj(st, "variantsActive");
        js::put(st.get_mut("variantsActive").unwrap(), &id, Some(cur))?;
        let mut parts = Vec::new();
        for u in updates {
            let c = js::prop(u, "channel")?;
            parts.push(format!("{}={}", js::to_string(c.as_ref()), js::to_string(js::get(u, "active").as_ref())));
        }
        app.log(&format!("variants {}: {}", js::to_string(item.as_ref()), parts.join(", ")));
    }
    // the locker slot carries the styles of what it holds (lockerSlots[slot].activeVariants[i].variants)
    let saved = if item_truthy { Some(styles_of(app, st, &js::lower(item.as_ref(), "item")?)?) } else { None };
    let saved_has = match &saved {
        Some(Value::Array(a)) => !a.is_empty(),
        Some(Value::String(s)) => !s.is_empty(),
        _ => false,
    };
    ensure_obj(st, "slotVariants");
    let svs = st.get_mut("slotVariants").unwrap();
    if js::get(svs, slot).map_or(true, |v| v.is_null()) {
        js::put(svs, slot, Some(Value::Array(vec![Value::Null; arr.len()])))?;
    }
    let mut detached = Value::Array(vec![Value::Null; arr.len()]);
    let sv = match svs {
        Value::Object(o) => o.get_mut(slot).unwrap_or(&mut detached),
        _ => &mut detached,
    };
    if let Value::Array(a) = sv {
        while a.len() < arr.len() {
            a.push(Value::Null);
        }
    } else if let Value::String(s) = sv {
        if s.encode_utf16().count() < arr.len() {
            return Err(type_error("sv.push is not a function"));
        }
    }
    for i in 0..arr.len() {
        if hit(i) {
            let val = if saved_has { slot_variant(saved.as_ref().unwrap())? } else { Value::Null };
            js::put(sv, &i.to_string(), Some(val))?;
        }
    }
    *changed = true;
    Ok(())
}

fn athena(app: &App, st: &Obj, info: &Info) -> JsResult<Value> {
    let mut items = Obj::new();
    for (tid, id) in &app.cos_items {
        let variants = styles_of(app, st, id)?;
        let fav = st.get("favorites").and_then(|f| js::get(f, id));
        js::set(
            &mut items,
            id,
            json!({ "templateId": tid, "attributes": { "max_level_bonus": 0, "level": 1, "item_seen": true, "xp": 0, "variants": variants,
                "favorite": js::truthy(fav.as_ref()), "rnd_sel_cnt": 0 }, "quantity": 1 }),
        );
    }
    for (_, d) in DEFAULTS {
        let k = d.to_lowercase();
        if items.get(&k).map_or(true, Value::is_null) {
            js::set(&mut items, &k, json!({ "templateId": d, "attributes": { "item_seen": true, "variants": [], "favorite": false }, "quantity": 1 }));
        }
    }
    let mut locker = Obj::new();
    for (slot, _) in SLOT_COUNTS {
        let sv = js::opt_chain(st.get("slotVariants"), slot);
        let active = js::or(sv, Value::Array(vec![Value::Null; slot_items(st, slot).len()]));
        locker.insert(slot.into(), json!({ "items": slot_items(st, slot), "activeVariants": active }));
    }
    let banner = st.get("banner").cloned().unwrap_or(Value::Null);
    let mut attrs = js::obj(json!({ "locker_slots_data": { "slots": locker }, "use_count": 0 }));
    js::put_opt(&mut attrs, "banner_icon_template", js::prop(&banner, "icon")?);
    js::put_opt(&mut attrs, "banner_color_template", js::prop(&banner, "color")?);
    js::put_opt(&mut attrs, "locker_name", st.get("lockerName").cloned());
    attrs.insert("item_seen".into(), json!(true));
    attrs.insert("favorite".into(), json!(false));
    js::set(&mut items, "sandbox_loadout", json!({ "templateId": "CosmeticLocker:cosmeticlocker_athena", "attributes": attrs, "quantity": 1 }));

    let s = js::num(info.season);
    let fav = |slot: &str| js::or(slot_items(st, slot).into_iter().next(), json!(""));
    let rvn = js::or(st.get("rvn").and_then(|r| js::get(r, "athena")), json!(1));
    let mut a = js::obj(json!({
        "use_random_loadout": false, "past_seasons": [], "season_match_boost": 0, "loadouts": ["sandbox_loadout"], "mfa_reward_claimed": true,
        "rested_xp_overflow": 0, "last_xp_interaction": js::now_iso(), "quest_manager": { "dailyLoginInterval": js::now_iso(), "dailyQuestRerolls": 1 },
        "book_level": 100, "season_num": s, "book_xp": 0, "permissions": [], "creative_dynamic_xp": {},
        "season": { "numWins": 0, "numHighBracket": 0, "numLowBracket": 0 }, "vote_data": {}, "lifetime_wins": 0, "book_purchased": true,
        "rested_xp_exchange": 1, "level": 100, "rested_xp": 0, "rested_xp_mult": 1, "accountLevel": 500, "xp": 0, "battlestars": 0,
        "battlestars_season_total": 0, "season_friend_match_boost": 0, "active_loadout_index": 0, "purchased_bp_offers": [],
        "purchased_battle_pass_tier_offers": [], "last_match_end_datetime": "", "mtx_purchase_history_copy": [], "last_applied_loadout": "sandbox_loadout",
        "favorite_character": fav("Character"), "favorite_backpack": fav("Backpack"), "favorite_pickaxe": fav("Pickaxe"), "favorite_glider": fav("Glider"),
        "favorite_skydivecontrail": fav("SkyDiveContrail"), "favorite_musicpack": fav("MusicPack"), "favorite_loadingscreen": fav("LoadingScreen"),
        "favorite_dance": slot_items(st, "Dance"), "favorite_itemwraps": slot_items(st, "ItemWrap"),
    }));
    js::put_opt(&mut a, "banner_icon", js::prop(&banner, "icon")?);
    js::put_opt(&mut a, "banner_color", js::prop(&banner, "color")?);
    a.extend(js::obj(json!({
        "favorite_victorypose": "", "favorite_consumableemote": "", "favorite_callingcard": "", "favorite_spray": [], "favorite_hat": "",
        "favorite_mapmarker": "", "party_assist_quest": "", "pinned_quest": "", "inventory_limit_bonus": 0, "daily_rewards": {},
        "competitive_identity": {}, "season_update": 0, "alien_style_points": 0,
    })));
    let mut p = js::obj(json!({ "_id": app.account_id, "created": SINCE, "updated": js::now_iso(), "rvn": rvn, "wipeNumber": 1,
        "accountId": app.account_id, "profileId": "athena", "version": "offline", "commandRevision": rvn }));
    p.insert("items".into(), Value::Object(items));
    p.insert("stats".into(), json!({ "attributes": a }));
    Ok(Value::Object(p))
}

fn common_core(app: &App, st: &Obj) -> Value {
    let rvn = js::or(st.get("rvn").and_then(|r| js::get(r, "common_core")), json!(1));
    json!({ "_id": app.account_id, "created": SINCE, "updated": js::now_iso(), "rvn": rvn, "wipeNumber": 1, "accountId": app.account_id,
        "profileId": "common_core", "version": "offline", "commandRevision": rvn,
        "items": { "Currency:MtxPurchased": { "templateId": "Currency:MtxPurchased", "attributes": { "platform": "EpicPC" }, "quantity": 0 } },
        "stats": { "attributes": { "survey_data": {}, "personal_offers": {}, "intro_game_played": true, "import_friends_claimed": {},
            "mtx_purchase_history": { "refundsUsed": 0, "refundCredits": 3, "purchases": [] }, "undo_cooldowns": [], "mtx_affiliate_set_time": "",
            "inventory_limit_bonus": 0, "current_mtx_platform": "EpicPC", "mtx_affiliate": "", "forced_intro_played": "Coconut",
            "weekly_purchases": {}, "daily_purchases": {}, "ban_history": {}, "in_app_purchases": {}, "permissions": [], "undo_timeout": "min",
            "monthly_purchases": {}, "allowed_to_send_gifts": false, "mfa_enabled": true, "allowed_to_receive_gifts": false, "gift_history": {} } } })
}

fn empty_profile(app: &App, st: &Obj, profile_id: &str) -> JsResult<Value> {
    let rvn = js::or(st.get("rvn").and_then(|r| js::get(r, profile_id)), json!(1));
    let mut base = js::obj(json!({ "_id": app.account_id, "created": SINCE, "updated": js::now_iso(), "rvn": rvn, "wipeNumber": 1,
        "accountId": app.account_id, "profileId": profile_id, "version": "offline", "commandRevision": rvn, "items": {}, "stats": { "attributes": {} } }));
    let attrs = match profile_id {
        "common_public" => {
            let banner = st.get("banner").cloned().unwrap_or(Value::Null);
            let mut a = Obj::new();
            js::put_opt(&mut a, "banner_color", js::prop(&banner, "color")?);
            a.insert("homebase_name".into(), json!(""));
            js::put_opt(&mut a, "banner_icon", js::prop(&banner, "icon")?);
            Some(a)
        }
        "creative" => Some(js::obj(json!({ "last_used_project": "", "max_island_plots": 50, "publish_allowed": false, "support_code": "",
            "last_used_plot": "", "permissions": [], "creator_name": "", "publish_banned": false, "inventory_limit_bonus": 0 }))),
        _ => None,
    };
    if let Some(a) = attrs {
        base.insert("stats".into(), json!({ "attributes": a }));
    }
    Ok(Value::Object(base))
}

fn profile_for(app: &App, st: &Obj, id: &str, info: &Info) -> JsResult<Value> {
    match id {
        "athena" => athena(app, st, info),
        "common_core" => Ok(common_core(app, st)),
        _ => empty_profile(app, st, id),
    }
}

/// mcp(command, profileId, body, info): apply the command, then answer the whole profile
pub fn mcp(app: &App, command: &str, profile_id: &str, body: &Value, info: &Info) -> JsResult<Value> {
    let mut guard = app.state.borrow_mut();
    let st: &mut Obj = &mut guard;
    let mut changed = false;
    match command {
        "EquipBattleRoyaleCustomization" | "SetCosmeticLockerSlot" => {
            let (cat, index) = if command == "SetCosmeticLockerSlot" { ("category", "slotIndex") } else { ("slotName", "indexWithinSlot") };
            let slot = slot_of(js::prop(body, cat)?.as_ref());
            let idx = js::nullish_or(js::prop(body, index)?, json!(0));
            let item = js::prop(body, "itemToSlot")?;
            let vu = js::prop(body, "variantUpdates")?;
            set_slot(app, st, slot, idx, item, vu, &mut changed)?;
        }
        "SetCosmeticLockerBanner" | "SetBattleRoyaleBanner" => {
            let banner = st.get("banner").cloned().unwrap_or(Value::Null);
            let bi = js::prop(body, "bannerIconTemplateName")?;
            let icon = if js::truthy(bi.as_ref()) { bi } else { js::prop(&banner, "icon")? };
            let bc = js::prop(body, "bannerColorTemplateName")?;
            let color = if js::truthy(bc.as_ref()) { bc } else { js::prop(&banner, "color")? };
            let mut o = Obj::new();
            js::put_opt(&mut o, "icon", icon);
            js::put_opt(&mut o, "color", color);
            js::set(st, "banner", Value::Object(o));
            changed = true;
        }
        "SetCosmeticLockerName" => {
            let n = js::or(js::prop(body, "name")?, json!(""));
            js::set(st, "lockerName", Value::String(js::to_string(Some(&n))));
            changed = true;
        }
        "SetItemFavoriteStatus" => {
            let t = js::prop(body, "targetItemId")?;
            if js::truthy(t.as_ref()) {
                let key = js::lower(t.as_ref(), "body.targetItemId")?;
                let fav = js::truthy(js::prop(body, "bFavorite")?.as_ref());
                match st.get_mut("favorites") {
                    Some(f) => js::put(f, &key, Some(Value::Bool(fav)))?,
                    None => return Err(type_error(format!("Cannot set properties of undefined (setting '{}')", key))),
                }
                changed = true;
            }
        }
        "SetItemFavoriteStatusBatch" => {
            let ids = js::or(js::prop(body, "itemIds")?, json!([]));
            let Value::Array(ids) = ids else {
                return Err(type_error("(body.itemIds || []).forEach is not a function"));
            };
            for (i, id) in ids.iter().enumerate() {
                let key = js::lower(Some(id), "id")?;
                let st_fav = js::or(js::prop(body, "itemFavStatus")?, json!([]));
                let fav = js::truthy(js::get(&st_fav, &i.to_string()).as_ref());
                match st.get_mut("favorites") {
                    Some(f) => js::put(f, &key, Some(Value::Bool(fav)))?,
                    None => return Err(type_error(format!("Cannot set properties of undefined (setting '{}')", key))),
                }
            }
            changed = true;
        }
        // PutModularCosmeticLoadout, SetRandomCosmeticLoadoutFlag, MarkItemSeen, QueryProfile, ClientQuestLogin, ...: nothing to change
        _ => {}
    }
    if changed {
        let next = js::or_one_plus_one(st.get("rvn").and_then(|r| js::get(r, profile_id)).as_ref());
        match st.get_mut("rvn") {
            Some(r) => js::put(r, profile_id, Some(next))?,
            None => return Err(type_error(format!("Cannot set properties of undefined (setting '{}')", profile_id))),
        }
        app.save_state(st);
    }
    let profile = profile_for(app, st, profile_id, info)?;
    let rvn = profile.get("rvn").cloned();
    let cmd = profile.get("commandRevision").cloned();
    let mut r = Obj::new();
    js::put_opt(&mut r, "profileRevision", rvn.clone());
    r.insert("profileId".into(), json!(profile_id));
    js::put_opt(&mut r, "profileChangesBaseRevision", rvn);
    r.insert("profileChanges".into(), json!([{ "changeType": "fullProfileUpdate", "profile": profile }]));
    js::put_opt(&mut r, "profileCommandRevision", cmd);
    r.insert("serverTime".into(), json!(js::now_iso()));
    r.insert("multiUpdate".into(), json!([]));
    r.insert("responseVersion".into(), json!(1));
    Ok(Value::Object(r))
}
