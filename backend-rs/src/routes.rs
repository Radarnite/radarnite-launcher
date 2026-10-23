//! The 105 routes of server.mjs (lines 240-436), registered in the same order: the first route whose method
//! matches and whose pattern matches the path, case-insensitively, answers.

use crate::app::{read_dir_names, read_file, read_json, App, Info, FAR, SINCE};
use crate::js::{self, type_error, JsError, JsResult, Obj};
use crate::mcp;
use serde_json::{json, Value};

pub enum Resp {
    /// application/json
    Json(u16, String),
    /// a body with its own Content-Type
    Body(u16, Vec<u8>, &'static str),
    /// 204, no body
    Empty,
}

pub struct Req {
    pub method: String,
    pub pathname: String,
    pub query: Option<String>,
    pub auth: Option<String>,
    pub body: Vec<u8>,
    /// the regex match: m[0] = the path, m[1..] = the :params
    pub m: Vec<String>,
    pub info: Info,
}

impl Req {
    /// url.searchParams.get(name)
    pub fn param(&self, name: &str) -> Option<String> {
        js::search_params(self.query.as_deref().unwrap_or("")).into_iter().find(|p| p.0 == name).map(|p| p.1)
    }
    /// url.searchParams.getAll(name)
    pub fn params(&self, name: &str) -> Vec<String> {
        js::search_params(self.query.as_deref().unwrap_or("")).into_iter().filter(|p| p.0 == name).map(|p| p.1).collect()
    }
    /// body.toString()
    pub fn body_str(&self) -> String {
        String::from_utf8_lossy(&self.body).into_owned()
    }
    /// JSON.parse(body.toString() || '{}'), {} on a syntax error
    pub fn body_json(&self) -> Value {
        let s = self.body_str();
        if s.is_empty() {
            json!({})
        } else {
            js::parse(&s).unwrap_or_else(|| json!({}))
        }
    }
}

pub type Handler = fn(&App, &Req) -> JsResult<Resp>;

enum Tok {
    Lit(&'static [u8]),
    /// `:name` -> ([^/]+)
    Param,
    /// `*` -> .*
    Star,
}

pub struct Route {
    pub method: &'static str,
    toks: Vec<Tok>,
    pub handler: Handler,
}

impl Route {
    /// `^pattern$` with the 'i' flag; Some(m) like String.prototype.match
    pub fn matches(&self, path: &str) -> Option<Vec<String>> {
        let mut caps = Vec::new();
        if match_at(&self.toks, path.as_bytes(), 0, &mut caps) {
            let mut m = vec![path.to_string()];
            m.extend(caps.iter().map(|&(a, b)| path[a..b].to_string()));
            Some(m)
        } else {
            None
        }
    }
}

fn match_at(toks: &[Tok], s: &[u8], at: usize, caps: &mut Vec<(usize, usize)>) -> bool {
    let rest = &s[at..];
    match toks.first() {
        None => rest.is_empty(),
        Some(Tok::Lit(l)) => {
            rest.len() >= l.len() && rest[..l.len()].eq_ignore_ascii_case(l) && match_at(&toks[1..], s, at + l.len(), caps)
        }
        Some(Tok::Param) => {
            let max = rest.iter().position(|&c| c == b'/').unwrap_or(rest.len());
            for len in (1..=max).rev() {
                caps.push((at, at + len));
                if match_at(&toks[1..], s, at + len, caps) {
                    return true;
                }
                caps.pop();
            }
            false
        }
        Some(Tok::Star) => {
            for len in (0..=rest.len()).rev() {
                if match_at(&toks[1..], s, at + len, caps) {
                    return true;
                }
            }
            false
        }
    }
}

/// pattern.replace(/:\w+/g, '([^/]+)').replace(/\*/g, '.*')
fn compile(p: &'static str) -> Vec<Tok> {
    let b = p.as_bytes();
    let word = |c: u8| c.is_ascii_alphanumeric() || c == b'_';
    let mut toks = Vec::new();
    let mut lit = 0;
    let mut i = 0;
    while i < b.len() {
        let special = (b[i] == b':' && i + 1 < b.len() && word(b[i + 1])) || b[i] == b'*';
        if !special {
            i += 1;
            continue;
        }
        if lit < i {
            toks.push(Tok::Lit(&b[lit..i]));
        }
        if b[i] == b'*' {
            toks.push(Tok::Star);
            i += 1;
        } else {
            toks.push(Tok::Param);
            i += 1;
            while i < b.len() && word(b[i]) {
                i += 1;
            }
        }
        lit = i;
    }
    if lit < b.len() {
        toks.push(Tok::Lit(&b[lit..]));
    }
    toks
}

fn on(method: &'static str, pattern: &'static str, handler: Handler) -> Route {
    Route { method, toks: compile(pattern), handler }
}

// ---------- answers ----------

pub fn json(code: u16, v: &Value) -> Resp {
    Resp::Json(code, js::stringify(v))
}
fn j(code: u16, v: Value) -> JsResult<Resp> {
    Ok(json(code, &v))
}
fn t(code: u16, text: &str, ctype: &'static str) -> JsResult<Resp> {
    Ok(Resp::Body(code, text.as_bytes().to_vec(), ctype))
}
fn e() -> JsResult<Resp> {
    Ok(Resp::Empty)
}
pub fn not_found(p: &str) -> Resp {
    json(
        404,
        &json!({ "errorCode": "errors.com.epicgames.common.not_found",
            "errorMessage": format!("Sorry the resource you were trying to find could not be found ({})", p),
            "messageVars": [p], "numericErrorCode": 1004, "originatingService": "any", "intent": "prod",
            "error_description": "not found", "error": "not_found" }),
    )
}

fn lightswitch() -> Value {
    json!({ "serviceInstanceId": "fortnite", "status": "UP", "message": "Fortnite is online", "maintenanceUri": null,
        "overrideCatalogIds": ["a7f138b2e51945ffbfdacc1af0541053"], "allowedActions": ["PLAY", "DOWNLOAD"], "banned": false,
        "launcherInfoDTO": { "appName": "Fortnite", "catalogItemId": "4fe75bbc5a674f4f9b356b5c90567da5", "namespace": "fn" } })
}

fn timeline(info: &Info) -> Value {
    let s = js::num(info.season);
    let ss = js::num_to_string(info.season);
    let events: Vec<Value> = [format!("EventFlag.Season{}", ss), format!("EventFlag.LobbySeason{}", ss)]
        .into_iter()
        .map(|event_type| json!({ "eventType": event_type, "activeUntil": FAR, "activeSince": SINCE }))
        .collect();
    json!({ "channels": {
        "client-matchmaking": { "states": [], "cacheExpire": FAR },
        "client-events": { "states": [{ "validFrom": SINCE, "activeEvents": events, "state": {
            "activeStorefronts": [], "eventNamedWeights": {}, "seasonNumber": s, "seasonTemplateId": format!("AthenaSeason:athenaseason{}", ss),
            "matchXpBonusPoints": 0, "seasonBegin": "2018-01-01T00:00:00Z", "seasonEnd": FAR, "seasonDisplayedEnd": FAR, "weeklyStoreEnd": FAR,
            "stwEventStoreEnd": FAR, "stwWeeklyStoreEnd": FAR, "sectionStoreEnds": {}, "dailyStoreEnd": FAR } }], "cacheExpire": FAR },
    }, "eventsTimeOffsetHrs": 0, "cacheIntervalMins": 10, "currentTime": js::now_iso() })
}

fn content(info: &Info) -> Value {
    let stage = format!("season{}", js::num_to_string(info.season));
    let now = js::now_iso;
    let msg = json!({ "title": "Offline", "body": format!("Fortnite {} offline (local backend). Nothing here talks to Epic.", info.version),
        "spotlight": false, "hidden": false, "messagetype": "normal", "_type": "CommonUI Simple Message Base" });
    let sub = || json!({ "_type": "CommonUI Simple Message", "message": { "image": "", "hidden": false, "messagetype": "normal",
        "_type": "CommonUI Simple Message Base", "title": "", "body": "", "spotlight": false } });
    json!({ "_title": "Fortnite Game", "_activeDate": SINCE, "lastModified": now(), "_locale": "en-US",
        "battleroyalenews": { "news": { "motds": [], "messages": [msg] }, "_title": "battleroyalenews", "header": "", "style": "None", "_noIndex": false,
            "alwaysShow": false, "_activeDate": SINCE, "lastModified": now(), "_locale": "en-US" },
        "battleroyalenewsv2": { "news": { "motds": [] }, "_title": "battleroyalenewsv2", "_activeDate": SINCE, "lastModified": now(), "_locale": "en-US" },
        "emergencynotice": { "news": { "platform_messages": [], "_type": "Battle Royale News", "messages": [] }, "_title": "emergencynotice",
            "_activeDate": SINCE, "lastModified": now(), "_locale": "en-US" },
        "emergencynoticev2": { "jcr:isCheckedOut": true, "_title": "emergencynoticev2", "_noIndex": false,
            "emergencynotices": { "_type": "Emergency Notices", "emergencynotices": [] }, "_activeDate": SINCE, "lastModified": now(), "_locale": "en-US" },
        "dynamicbackgrounds": { "backgrounds": { "backgrounds": [{ "stage": stage, "_type": "DynamicBackground", "key": "lobby" },
            { "stage": stage, "_type": "DynamicBackground", "key": "vault" }], "_type": "DynamicBackgroundList" }, "_title": "dynamicbackgrounds",
            "_noIndex": false, "_activeDate": SINCE, "lastModified": now(), "_locale": "en-US" },
        "shopSections": { "sectionList": { "_type": "ShopSectionList", "sections": [] }, "_title": "shop-sections", "_activeDate": SINCE,
            "lastModified": now(), "_locale": "en-US" },
        "playlistinformation": { "conversion_config": { "containerName": "playlist_info", "_type": "Conversion Config", "contentName": "playlists" },
            "playlist_info": { "_type": "Playlist Information", "playlists": [] }, "_title": "playlistinformation", "_activeDate": SINCE,
            "lastModified": now(), "_locale": "en-US" },
        "tournamentinformation": { "tournament_info": { "tournaments": [], "_type": "Tournaments Info" }, "_title": "tournamentinformation",
            "_activeDate": SINCE, "lastModified": now(), "_locale": "en-US" },
        "subgameselectdata": { "saveTheWorldUnowned": sub(), "_title": "subgameselectdata", "battleRoyale": sub(), "creative": sub(),
            "saveTheWorld": sub(), "_activeDate": SINCE, "lastModified": now(), "_locale": "en-US" },
        "lobby": { "backgroundimage": "", "stage": stage, "_title": "lobby", "_activeDate": SINCE, "lastModified": now(), "_locale": "en-US" },
    })
}

fn hotfix_entry(name: &str, content: &str) -> Value {
    let b = content.as_bytes();
    json!({ "uniqueFilename": name, "filename": name, "hash": js::hex(&js::sha1(b)), "hash256": js::sha256_hex(b), "length": b.len(),
        "contentType": "application/octet-stream", "uploaded": "2021-01-01T00:00:00.000Z", "storageType": "S3", "storageIds": {}, "doNotCache": true })
}

/// HOTFIX[n] for names Object.prototype answers: Buffer.byteLength(<function>) throws
fn proto_hotfix(n: &str) -> Option<String> {
    const FNS: [&str; 11] = ["__defineGetter__", "__defineSetter__", "hasOwnProperty", "__lookupGetter__", "__lookupSetter__",
        "isPrototypeOf", "propertyIsEnumerable", "toString", "valueOf", "toLocaleString", "constructor"];
    let pre = "The \"string\" argument must be of type string or an instance of Buffer or ArrayBuffer. Received ";
    if n == "__proto__" {
        return Some(format!("{}an instance of Object", pre));
    }
    FNS.iter().find(|f| **f == n).map(|f| format!("{}function {}", pre, if *f == "constructor" { "Object" } else { f }))
}

// ---------- modes in the game's own menu (lobby "CHANGE": the Discovery surface) ----------
const MODE_LINKS: [(&str, &str); 5] = [
    ("playlist_playground", "cm"),
    ("playlist_defaultsolo", "br"),
    ("playlist_defaultduo", "br"),
    ("playlist_trios", "br"),
    ("playlist_defaultsquad", "br"),
];

fn link_data(l: &(&str, &str)) -> Value {
    json!({ "namespace": "fn", "mnemonic": l.0, "linkType": "BR:Playlist", "active": true, "disabled": false, "version": 1,
        "moderationStatus": "Unmoderated", "accountId": "epic", "creatorName": "Epic", "descriptionTags": [],
        "metadata": { "image_url": format!("https://cdn2.unrealengine.com/offline/ui/{}.jpg", l.1), "matchmaking": { "override_playlist": l.0 } } })
}

fn surface_asset() -> Value {
    json!({ "FortCreativeDiscoverySurface": { "meta": { "promotion": 1 }, "assets": { "CreativeDiscoverySurface_Frontend": {
        "meta": { "revision": 1, "headRevision": 1, "revisedAt": "2021-01-01T00:00:00.000Z", "promotion": 1, "promotedAt": "2021-01-01T00:00:00.000Z" },
        "assetData": { "AnalyticsId": "offline", "SurfaceName": "CreativeDiscoverySurface_Frontend", "TestName": "offline",
            "primaryAssetId": "FortCreativeDiscoverySurface:CreativeDiscoverySurface_Frontend", "GlobalLinkCodeBlacklist": [], "GlobalLinkCodeWhitelist": [],
            "TestCohorts": [{ "AnalyticsId": "offline", "CohortSelector": "PlayerDeterministic", "PlatformBlacklist": [], "PlatformWhitelist": [],
                "SelectionChance": 1, "TestName": "offline",
                "ContentPanels": [{ "NumPages": 1, "AnalyticsId": "offline", "PanelType": "AnalyticsList", "AnalyticsListName": "ByEpicWoven",
                    "CuratedListOfLinkCodes": [], "ModelName": "", "PageSize": MODE_LINKS.len(), "PlatformBlacklist": [], "PlatformWhitelist": [],
                    "PanelName": "ByEpicWoven", "MetricInterval": "", "SkippedEntriesCount": 0, "SkippedEntriesPercent": 0, "SplicedEntries": [],
                    "EntrySkippingMethod": "None", "PlayHistoryType": "RecentlyPlayed", "bLowestToHighest": false, "PanelLinkCodeBlacklist": [],
                    "PanelLinkCodeWhitelist": [], "FeatureTags": [], "MetricName": "",
                    "PanelDisplayName": { "Category": "Game", "NativeCulture": "", "Namespace": "CreativeDiscoverySurface_Frontend", "LocalizedStrings": [],
                        "bIsMinimalPatch": false, "NativeString": "Offline", "Key": "ByEpicWoven" } }] }] } } } } })
}

fn surface() -> Value {
    let results: Vec<Value> = MODE_LINKS
        .iter()
        .map(|l| json!({ "linkData": link_data(l), "lastVisited": null, "linkCode": l.0, "isFavorite": false }))
        .collect();
    json!({ "Panels": [{ "PanelName": "ByEpicWoven", "Pages": [{ "results": results, "hasMore": false }] }], "TestCohorts": ["offline"], "ModeSets": {} })
}

fn stats(a: &App) -> Value {
    json!({ "startTime": 0, "endTime": 0, "stats": {}, "accountId": a.account_id })
}

fn privacy(a: &App) -> Value {
    json!({ "accountId": a.account_id, "optOutOfPublicLeaderboards": false })
}

/// `/^bearer\s+/i` removed
fn strip_bearer(s: &str) -> String {
    if s.len() >= 6 && s.as_bytes()[..6].eq_ignore_ascii_case(b"bearer") {
        let rest = &s[6..];
        let trimmed = rest.trim_start_matches(|c: char| {
            matches!(c, '\t' | '\n' | '\u{b}' | '\u{c}' | '\r' | ' ' | '\u{a0}' | '\u{1680}' | '\u{2000}'..='\u{200a}'
                | '\u{2028}' | '\u{2029}' | '\u{202f}' | '\u{205f}' | '\u{3000}' | '\u{feff}')
        });
        if trimmed.len() < rest.len() {
            return trimmed.to_string();
        }
    }
    s.to_string()
}

fn session(a: &App, id: &str) -> Value {
    let w = a.wanted.borrow().clone();
    let g = a.server_for(&w).or_else(|| {
        let gv = &a.game_server.borrow().v;
        if js::truthy(Some(gv)) {
            Some(gv.clone())
        } else {
            None
        }
    });
    let (addr, port) = match &g {
        None => (Some(json!("127.0.0.1")), Some(json!(7777))),
        Some(Value::Object(o)) => (o.get("address").cloned(), o.get("port").cloned()),
        Some(Value::Array(_)) => (Some(json!("127.0.0.1")), None),
        Some(_) => (None, None),
    };
    let mut o = js::obj(json!({ "id": id, "ownerId": a.server_id, "ownerName": "OfflineServer", "serverName": "OfflineServer" }));
    js::put_opt(&mut o, "serverAddress", addr);
    js::put_opt(&mut o, "serverPort", port);
    let playlist = if w.playlist.is_empty() { "playlist_defaultsolo".to_string() } else { w.playlist.clone() };
    let build_unique_id = a.build_unique_id.borrow().clone();
    o.extend(js::obj(json!({ "maxPublicPlayers": 220, "openPublicPlayers": 175, "maxPrivatePlayers": 0, "openPrivatePlayers": 0,
        "attributes": { "REGION_s": "EU", "GAMEMODE_s": "FORTATHENA", "ALLOWBROADCASTING_b": true, "SUBREGION_s": "GB",
            "DCID_s": "FORTNITE-LIVEEUGCEC1C2E30UBRCORE0A-14840880", "tenant_s": "Fortnite", "MATCHMAKINGPOOL_s": "Any", "STORMSHIELDDEFENSETYPE_i": 0,
            "HOTFIXVERSION_i": 0, "PLAYLISTNAME_s": playlist, "SESSIONKEY_s": "offline", "TENANT_s": "Fortnite", "BEACONPORT_i": 15009 },
        "publicPlayers": [], "privatePlayers": [], "totalPlayers": 0, "allowJoinInProgress": false, "shouldAdvertise": false, "isDedicated": false,
        "usesStats": false, "allowInvites": false, "usesPresence": false, "allowJoinViaPresence": true, "allowJoinViaPresenceFriendsOnly": false,
        "buildUniqueId": build_unique_id, "lastUpdated": js::now_iso(), "started": false })));
    Value::Object(o)
}

fn server_ready(a: &App, q: &Req) -> JsResult<Resp> {
    let parsed = js::parse(&q.body_str()).unwrap_or_else(|| json!({ "port": 7777 }));
    {
        let mut g = a.game_server.borrow_mut();
        let g = &mut *g;
        g.v = parsed;
        g.ready_at = None;
        match &mut g.v {
            Value::Object(o) => {
                let now = js::now_ms();
                js::set(o, "address", json!("127.0.0.1"));
                js::set(o, "readyAt", json!(now));
                g.ready_at = Some(now);
            }
            Value::Array(_) => g.ready_at = Some(js::now_ms()),
            Value::Null => return Err(type_error("Cannot set properties of null (setting 'address')")),
            other => {
                return Err(type_error(format!(
                    "Cannot create property 'address' on {} '{}'",
                    js::type_name(other),
                    js::to_string(Some(other))
                )))
            }
        }
    }
    a.log(&format!("game server ready {}", js::stringify(&a.game_server.borrow().v)));
    e()
}

fn ticket(a: &App, q: &Req) -> JsResult<Resp> {
    // The session must carry the client's own build id (first field of bucketId), or the client rejects it (FindSessionFailure).
    let bucket = q.param("bucketId").unwrap_or_default();
    let parts: Vec<&str> = bucket.split(':').collect();
    if !parts[0].is_empty() && parts[0].bytes().all(|c| c.is_ascii_digit()) {
        *a.build_unique_id.borrow_mut() = parts[0].to_string();
    }
    if a.no_match {
        return j(403, json!({ "errorCode": "errors.com.epicgames.offline.no_matchmaking",
            "errorMessage": "Offline: this build has no local match server (lobby only).", "numericErrorCode": 1000,
            "originatingService": "fortnite", "intent": "prod" }));
    }
    let pl = parts.get(3).filter(|s| !s.is_empty()).copied().unwrap_or("playlist_defaultsolo").to_lowercase();
    let seq = {
        let mut w = a.wanted.borrow_mut();
        *w = crate::app::Wanted { playlist: pl.clone(), seq: w.seq + 1, at: js::now_ms() };
        w.seq
    };
    a.log(&format!("matchmaker: wanted {} #{}", pl, seq));
    j(200, json!({ "serviceUrl": format!("ws://127.0.0.1:{}/matchmaking", js::num_to_string(a.port)), "ticketType": "mms-player",
        "payload": "offline", "signature": "offline" }))
}

fn party(a: &App, q: &Req) -> JsResult<Resp> {
    let jv = q.body_json();
    let cfg = js::prop(&jv, "config")?;
    let id = js::random_hex(16);
    let mut config = Obj::new();
    if js::truthy(cfg.as_ref()) {
        js::spread_into(&mut config, cfg.as_ref().unwrap());
    }
    for (k, v) in [("type", json!("DEFAULT")), ("joinability", json!("OPEN")), ("discoverability", json!("ALL")), ("sub_type", json!("default")),
        ("max_size", json!(16)), ("invite_ttl", json!(14400)), ("join_confirmation", json!(false))]
    {
        js::set(&mut config, k, v);
    }
    let ji = js::prop(&jv, "join_info")?;
    let conn = js::opt_chain(ji.as_ref(), "connection");
    let meta = js::or(js::opt_chain(ji.as_ref(), "meta"), json!({}));
    let cid = js::or(js::opt_chain(conn.as_ref(), "id"), json!(""));
    let cmeta = js::or(js::opt_chain(conn.as_ref(), "meta"), json!({}));
    let created = js::now_iso();
    let updated = js::now_iso();
    let connected = js::now_iso();
    let conn_updated = js::now_iso();
    let member_updated = js::now_iso();
    let joined = js::now_iso();
    let pmeta = js::or(js::prop(&jv, "meta")?, json!({}));
    j(200, json!({ "id": id, "created_at": created, "updated_at": updated, "config": config,
        "members": [{ "account_id": a.account_id, "meta": meta, "connections": [{ "id": cid, "connected_at": connected, "updated_at": conn_updated,
            "yield_leadership": false, "meta": cmeta }], "revision": 0, "updated_at": member_updated, "joined_at": joined, "role": "CAPTAIN" }],
        "applicants": [], "meta": pmeta, "invites": [], "revision": 0, "intentions": [] }))
}

fn cloud_list(a: &App, _q: &Req) -> JsResult<Resp> {
    let d = a.off.join("cloud");
    let files = if d.exists() { read_dir_names(&d).map_err(|e| crate::app::fs_error(&e, "scandir", &d))? } else { vec![] };
    let mut out = Vec::new();
    for f in files {
        let b = read_file(&d.join(&f))?;
        out.push(json!({ "uniqueFilename": f, "filename": f, "hash": js::hex(&js::sha1(&b)), "hash256": js::sha256_hex(&b), "length": b.len(),
            "contentType": "application/octet-stream", "uploaded": js::now_iso(), "storageType": "S3", "storageIds": {},
            "accountId": a.account_id, "doNotCache": false }));
    }
    j(200, Value::Array(out))
}

fn cloud_put(a: &App, q: &Req) -> JsResult<Resp> {
    let d = a.off.join("cloud");
    std::fs::create_dir_all(&d).map_err(|e| crate::app::fs_error(&e, "mkdir", &d))?;
    if q.body.len() < 4 * 1024 * 1024 {
        let n = js::decode_uri_component(&q.m[2])?;
        let f = d.join(js::basename(&n));
        std::fs::write(&f, &q.body).map_err(|e| {
            if f.is_dir() {
                JsError { name: "Error", message: format!("EISDIR: illegal operation on a directory, open '{}'", f.display()) }
            } else {
                crate::app::fs_error(&e, "open", &f)
            }
        })?;
    }
    e()
}

fn keychain(a: &App, _q: &Req) -> JsResult<Resp> {
    let k = read_json(&a.off.join("keychain.json")).unwrap_or_else(|| json!([]));
    let mut out = js::spread(Some(&k), "readJson(path.join(OFF, 'keychain.json'), [])")?;
    let ck = a.cos.keychain.as_ref();
    if js::truthy(ck) {
        out.extend(js::spread(ck, "customCosmetics.keychain")?);
    }
    j(200, Value::Array(out))
}

fn lock_status(_a: &App, q: &Req) -> JsResult<Resp> {
    let jv = q.body_json();
    let codes = js::or(js::prop(&jv, "linkCodes")?, json!([]));
    let Value::Array(codes) = codes else {
        return Err(type_error("(j.linkCodes || []).map is not a function"));
    };
    let results: Vec<Value> = codes
        .into_iter()
        .map(|c| json!({ "playerId": q.m[1], "linkCode": c, "lockStatus": "UNLOCKED", "lockStatusReason": "NONE", "isVisible": true }))
        .collect();
    j(200, json!({ "results": results, "hasMore": false }))
}

fn ui_file(a: &App, q: &Req) -> JsResult<Resp> {
    let f = a.off.join("ui").join(js::basename(&q.m[1]));
    if !f.exists() {
        return Ok(not_found(&q.m[1]));
    }
    let b = read_file(&f)?;
    let png = f.to_string_lossy().ends_with(".png");
    Ok(Resp::Body(200, b, if png { "image/png" } else { "image/jpeg" }))
}

pub fn table() -> Vec<Route> {
    vec![
        on("GET", "/offline/health", |a, _| {
            j(200, json!({ "ok": true, "accountId": a.account_id, "displayName": a.display_name, "cosmetics": a.cos.count,
                "build": js::basename(&a.build), "source": a.cos.source }))
        }),
        // accounts
        on("POST", "/account/api/oauth/token", |a, q| {
            let b = q.body_str();
            let f = js::search_params(b.strip_prefix('?').unwrap_or(&b));
            let user = f.into_iter().find(|p| p.0 == "username").map(|p| p.1).unwrap_or_default();
            j(200, a.token(&user))
        }),
        on("GET", "/account/api/oauth/verify", |a, q| {
            let t = strip_bearer(q.auth.as_deref().unwrap_or(""));
            let id = a.tokens.borrow().get(&t).cloned().unwrap_or_else(|| a.account_id.clone());
            let mut tk = js::obj(a.token(if id == a.server_id { "server@" } else { "" }));
            let token = if t.is_empty() { tk.get("access_token").cloned().unwrap_or(Value::Null) } else { json!(t) };
            js::set(&mut tk, "token", token);
            js::set(&mut tk, "account_id", json!(id));
            js::set(&mut tk, "session_id", json!("offline"));
            js::set(&mut tk, "auth_method", json!("password"));
            j(200, Value::Object(tk))
        }),
        on("DELETE", "/account/api/oauth/sessions/kill*", |_, _| e()),
        on("GET", "/account/api/oauth/exchange", |_, _| {
            j(200, json!({ "expiresInSeconds": 300, "code": "offline-exchange", "creatingClientId": "ec684b8c687f479fadea3cb2ad83f5c6" }))
        }),
        on("GET", "/account/api/public/account/displayName/:name", |a, _| j(200, a.public_account())),
        on("GET", "/account/api/public/account/:id/externalAuths", |_, _| j(200, json!([]))),
        on("GET", "/account/api/public/account/:id/deviceAuth", |_, _| j(200, json!([]))),
        on("GET", "/account/api/public/account/:id", |a, q| {
            let mut acc = a.account();
            if q.m[1] == a.server_id {
                js::set(&mut acc, "id", json!(a.server_id));
                js::set(&mut acc, "displayName", json!("OfflineServer"));
                js::set(&mut acc, "name", json!("OfflineServer"));
            }
            j(200, Value::Object(acc))
        }),
        on("GET", "/account/api/public/account", |a, q| {
            let list: Vec<Value> = q
                .params("accountId")
                .into_iter()
                .map(|id| if id == a.server_id { json!({ "id": id, "displayName": "OfflineServer", "externalAuths": {} }) } else { a.public_account() })
                .collect();
            j(200, Value::Array(list))
        }),
        on("GET", "/account/api/epicdomains/ssodomains", |_, _| j(200, json!([]))),
        on("POST", "/fortnite/api/game/v2/tryPlayOnPlatform/account/*", |_, _| t(200, "true", "text/plain")),
        on("GET", "/fortnite/api/game/v2/enabled_features", |_, _| j(200, json!([]))),
        on("POST", "/fortnite/api/game/v2/grant_access/*", |_, _| e()),
        on("POST", "/fortnite/api/storeaccess/v1/request_access/*", |_, _| e()),
        on("GET", "/eulatracking/api/shared/agreements/fn*", |_, _| e()),
        on("GET", "/eulatracking/api/public/agreements/*", |_, _| e()),
        on("GET", "/socialban/api/public/v1/*", |_, _| j(200, json!({ "bans": [], "warnings": [] }))),
        on("GET", "/waitingroom/api/waitingroom", |_, _| e()),
        on("GET", "/lightswitch/api/service/bulk/status", |_, _| j(200, json!([lightswitch()]))),
        on("GET", "/lightswitch/api/service/:svc/status", |_, _| j(200, lightswitch())),
        // versions
        on("GET", "/fortnite/api/version", |_, _| {
            j(200, json!({ "app": "fortnite", "serverDate": js::now_iso(), "overridePropertiesVersion": "unknown", "cln": "0", "build": "0",
                "moduleName": "Fortnite-Core", "buildDate": "2021-01-01T00:00:00.000Z", "version": "offline", "branch": "offline", "modules": {} }))
        }),
        on("GET", "/fortnite/api/v2/versioncheck*", |_, _| j(200, json!({ "type": "NO_UPDATE" }))),
        on("GET", "/fortnite/api/versioncheck*", |_, _| j(200, json!({ "type": "NO_UPDATE" }))),
        // calendar / content / cloud storage
        on("GET", "/fortnite/api/calendar/v1/timeline", |_, q| j(200, timeline(&q.info))),
        on("GET", "/content/api/pages/fortnite-game*", |_, q| j(200, content(&q.info))),
        on("GET", "/fortnite/api/cloudstorage/system", |a, _| {
            j(200, Value::Array(a.hotfix.iter().map(|(n, c)| hotfix_entry(n, c)).collect()))
        }),
        on("GET", "/fortnite/api/cloudstorage/system/config", |_, _| j(200, json!({}))),
        on("GET", "/fortnite/api/cloudstorage/system/:file", |a, q| {
            let n = js::decode_uri_component(&q.m[1])?;
            if let Some(msg) = proto_hotfix(&n) {
                return Err(JsError { name: "TypeError [ERR_INVALID_ARG_TYPE]", message: msg });
            }
            match a.hotfix.iter().find(|e| e.0 == n) {
                Some((_, c)) if !c.is_empty() => t(200, c, "application/octet-stream"),
                _ => Ok(not_found(&n)),
            }
        }),
        on("GET", "/fortnite/api/cloudstorage/user/:id/:file", |a, q| {
            let f = a.off.join("cloud").join(js::basename(&js::decode_uri_component(&q.m[2])?));
            if f.exists() {
                Ok(Resp::Body(200, read_file(&f)?, "application/octet-stream"))
            } else {
                e()
            }
        }),
        on("GET", "/fortnite/api/cloudstorage/user/:id", cloud_list),
        on("PUT", "/fortnite/api/cloudstorage/user/:id/:file", cloud_put),
        // profiles
        on("POST", "/fortnite/api/game/v2/profile/:id/:kind/:command", |a, q| {
            let body = q.body_json();
            let pid = q.param("profileId").filter(|s| !s.is_empty()).unwrap_or_else(|| "athena".into());
            Ok(Resp::Json(200, mcp::mcp(a, &q.m[3], &pid, &body, &q.info)?))
        }),
        // store / economy (nothing to buy: everything is owned)
        on("GET", "/fortnite/api/storefront/v2/catalog", |_, _| {
            j(200, json!({ "refreshIntervalHrs": 24, "dailyPurchaseHrs": 24, "expiration": FAR, "storefronts": [] }))
        }),
        on("GET", "/fortnite/api/storefront/v2/keychain", keychain),
        on("GET", "/fortnite/api/storefront/v2/gift/check_eligibility/*", |_, _| {
            j(200, json!({ "price": { "currencyType": "MtxCurrency", "currencySubType": "", "regularPrice": 0, "finalPrice": 0, "saleExpiration": FAR,
                "basePrice": 0 }, "items": [] }))
        }),
        on("GET", "/fortnite/api/receipts/v1/account/:id/receipts", |_, _| j(200, json!([]))),
        on("GET", "/catalog/api/shared/bulk/offers", |_, _| j(200, json!({}))),
        on("GET", "/fortnite/api/game/v2/br-inventory/account/:id", |_, _| j(200, json!({ "stash": { "globalcash": 0 } }))),
        on("GET", "/fortnite/api/game/v2/privacy/account/:id", |a, _| j(200, privacy(a))),
        on("POST", "/fortnite/api/game/v2/privacy/account/:id", |a, _| j(200, privacy(a))),
        on("GET", "/fortnite/api/game/v2/world/info", |_, _| j(200, json!({}))),
        on("GET", "/fortnite/api/game/v2/events/*", |_, _| j(200, json!([]))),
        on("GET", "/api/v1/events/Fortnite/download/:id*", |a, _| {
            j(200, json!({ "player": { "gameId": "Fortnite", "accountId": a.account_id, "tokens": [], "teams": {}, "pendingPayouts": [],
                "pendingPenalties": {}, "persistentScores": {}, "groupIdentity": {} }, "events": [], "templates": [], "scores": [] }))
        }),
        on("GET", "/api/v1/events/Fortnite/*", |_, _| j(200, json!([]))),
        on("GET", "/fortnite/api/statsv2/account/:id", |a, _| j(200, stats(a))),
        on("GET", "/statsproxy/api/statsv2/account/:id", |a, _| j(200, stats(a))),
        on("POST", "/fortnite/api/statsv2/query", |_, _| j(200, json!([]))),
        on("POST", "/statsproxy/api/statsv2/query", |_, _| j(200, json!([]))),
        on("GET", "/fortnite/api/stats/accountId/:id/bulk/window/alltime", |_, _| j(200, json!([]))),
        on("GET", "/fortnite/api/game/v2/leaderboards/*", |_, _| j(200, json!({ "entries": [] }))),
        on("GET", "/fortnite/api/game/v2/creative/*", |_, _| j(200, json!({ "results": [], "hasMore": false }))),
        on("GET", "/fortnite/api/game/v2/twitch/*", |_, _| e()),
        on("POST", "/fortnite/api/feedback/*", |_, _| e()),
        on("POST", "/fortnite/api/game/v2/profileToken/verify/:id", |_, _| e()),
        on("GET", "/api/v2/interactions/aggregated/Fortnite/:id", |_, _| j(200, json!([]))),
        on("GET", "/api/v2/interactions/latest/Fortnite/:id", |_, _| j(200, json!([]))),
        on("GET", "/api/v1/Fortnite/get", |_, _| j(200, json!({}))),
        on("POST", "/fortnite/api/game/v2/chat/*", |_, _| j(200, json!({ "GlobalChatRooms": [] }))),
        // match server on demand: the launcher polls /offline/wanted and posts server-ready / server-down
        on("POST", "/offline/server-ready", server_ready),
        on("POST", "/offline/server-down", |a, _| {
            let mut g = a.game_server.borrow_mut();
            if js::truthy(Some(&g.v)) {
                a.log("game server down");
            }
            g.v = Value::Null;
            g.ready_at = None;
            e()
        }),
        on("GET", "/offline/wanted", |a, _| {
            let w = a.wanted.borrow();
            j(200, json!({ "playlist": w.playlist, "seq": w.seq, "at": w.at, "server": a.game_server.borrow().v }))
        }),
        on("GET", "/fortnite/api/game/v2/matchmakingservice/ticket/player/*", ticket),
        on("GET", "/fortnite/api/game/v2/matchmaking/account/:id/session/:sid", |_, q| {
            j(200, json!({ "accountId": q.m[1], "sessionId": q.m[2], "key": "offline" }))
        }),
        on("GET", "/fortnite/api/matchmaking/session/:sid", |a, q| j(200, session(a, &q.m[1]))),
        on("POST", "/fortnite/api/matchmaking/session/:sid/join", |_, _| e()),
        on("POST", "/fortnite/api/matchmaking/session/matchMakingRequest", |_, _| j(200, json!([]))),
        on("GET", "/fortnite/api/matchmaking/session/findPlayer/*", |_, _| e()),
        on("GET", "/fortnite/api/discovery/accessToken/*", |_, _| j(200, json!({ "branchName": "offline", "appId": "Fortnite", "token": "offline" }))),
        // modes in the game's own menu
        on("POST", "/fortnite/api/game/v2/creative/discovery/surface/*", |_, _| j(200, surface())),
        on("POST", "/discovery/surface/*", |_, _| j(200, surface())),
        on("POST", "/api/v1/assets/Fortnite/*", |_, _| j(200, surface_asset())),
        on("POST", "/links/api/fn/mnemonic", |_, _| j(200, Value::Array(MODE_LINKS.iter().map(link_data).collect()))),
        on("GET", "/links/api/fn/mnemonic/:code/related", |_, q| {
            let c = q.m[1].to_lowercase();
            let mut links = Obj::new();
            if let Some(l) = MODE_LINKS.iter().find(|l| l.0 == c) {
                js::set(&mut links, l.0, link_data(l));
            }
            j(200, json!({ "parentLinks": [], "links": links }))
        }),
        on("GET", "/links/api/fn/mnemonic/:code", |_, q| {
            let c = q.m[1].to_lowercase();
            match MODE_LINKS.iter().find(|l| l.0 == c) {
                Some(l) => j(200, link_data(l)),
                None => Ok(not_found(&q.m[1])),
            }
        }),
        on("POST", "/api/v1/links/lock-status/:id/check", lock_status),
        on("GET", "/offline/ui/:file", ui_file),
        // friends / party / presence (solo lobby)
        on("GET", "/friends/api/public/friends/:id", |_, _| j(200, json!([]))),
        on("GET", "/friends/api/v1/:id/summary", |_, _| {
            j(200, json!({ "friends": [], "incoming": [], "outgoing": [], "suggested": [], "blocklist": [], "settings": { "acceptInvites": "public" } }))
        }),
        on("GET", "/friends/api/v1/:id/friends", |_, _| j(200, json!([]))),
        on("GET", "/friends/api/v1/:id/blocklist", |_, _| j(200, json!([]))),
        on("GET", "/friends/api/v1/:id/settings", |_, _| j(200, json!({ "acceptInvites": "public" }))),
        on("GET", "/friends/api/public/blocklist/:id", |_, _| j(200, json!({ "blockedUsers": [] }))),
        on("GET", "/friends/api/public/list/fortnite/:id/recentPlayers", |_, _| j(200, json!([]))),
        on("GET", "/friends/api/v1/:id/recent/fortnite", |_, _| j(200, json!([]))),
        on("GET", "/party/api/v1/Fortnite/user/:id", |_, _| j(200, json!({ "current": [], "pending": [], "invites": [], "pings": [] }))),
        on("GET", "/party/api/v1/Fortnite/user/:id/notifications/undelivered/count", |_, _| j(200, json!({ "pings": 0, "invites": 0 }))),
        on("POST", "/party/api/v1/Fortnite/parties", party),
        on("PATCH", "/party/api/v1/Fortnite/parties/*", |_, _| e()),
        on("DELETE", "/party/api/v1/Fortnite/parties/*", |_, _| e()),
        on("POST", "/party/api/v1/Fortnite/parties/*", |_, _| e()),
        on("GET", "/presence/api/v1/_/:id/settings/subscriptions", |_, _| j(200, json!([]))),
        on("GET", "/presence/api/v1/:ns/:id/last-online", |_, _| j(200, json!({}))),
        on("GET", "/presence/api/v1/*", |_, _| j(200, json!([]))),
        on("POST", "/presence/api/v1/*", |_, _| e()),
        // telemetry and misc: accepted and dropped locally
        on("POST", "/datarouter/api/v1/public/data*", |_, _| e()),
        on("POST", "/telemetry/*", |_, _| e()),
        on("GET", "/region", |_, _| {
            j(200, json!({ "continent": { "code": "EU", "geoname_id": 6255148, "names": { "en": "Europe" } },
                "country": { "iso_code": "ES", "names": { "en": "Spain" } }, "subdivisions": [] }))
        }),
        on("GET", "/content-controls/*", |_, _| j(200, json!({}))),
        on("POST", "/content-controls/*", |_, _| j(200, json!({}))),
        on("GET", "/hotconfigs/v2/*", |_, _| j(200, json!({}))),
        on("GET", "/api/v1/user/setting", |_, _| j(200, json!([]))),
        on("POST", "/api/v1/user/setting", |_, _| j(200, json!([]))),
        on("GET", "/fortnite/api/game/v2/homebase/*", |_, _| j(200, json!({}))),
        on("GET", "/affiliate/api/public/affiliates/slug/*", |_, _| Ok(not_found("affiliate"))),
    ]
}
