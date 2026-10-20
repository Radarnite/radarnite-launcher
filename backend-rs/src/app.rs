//! Startup and shared state of the backend (server.mjs lines 16-67 and the request loop at 439-456).

use crate::js::{self, type_error, JsError, JsResult, Obj};
use crate::routes::{self, Req, Resp, Route};
use serde_json::{json, Value};
use std::cell::RefCell;
use std::collections::{HashMap, HashSet};
use std::io::Write;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::path::{Path, PathBuf};

pub const FAR: &str = "9999-12-31T23:59:59.999Z";
pub const SINCE: &str = "2018-01-01T00:00:00.000Z";

/// The client's build and season, from its User-Agent ("Fortnite/++Fortnite+Release-17.50-CL-17388565 Windows/10").
#[derive(Clone, Debug)]
pub struct Info {
    pub season: f64,
    pub version: String,
}

impl Info {
    pub fn from_ua(ua: &str) -> Info {
        let b = ua.as_bytes();
        let mut from = 0;
        while let Some(p) = find(&b[from..], b"Release-") {
            if let Some((major, minor, cl)) = release_at(b, from + p + 8) {
                // Season number as the game counts it: 1.x is season 1/2 (by CL), afterwards season == major version.
                let season = if major == 1.0 {
                    if cl <= 3724489.0 {
                        1.0
                    } else {
                        2.0
                    }
                } else if major == 2.0 {
                    2.0
                } else {
                    major
                };
                let version = format!("{}.{}", js::num_to_string(major), js::num_to_string(minor));
                return Info { season, version };
            }
            from += p + 1;
        }
        Info { season: 0.0, version: "?".into() }
    }
}

fn find(h: &[u8], n: &[u8]) -> Option<usize> {
    h.windows(n.len()).position(|w| w == n)
}

fn digits(b: &[u8], i: usize) -> usize {
    b[i.min(b.len())..].iter().take_while(|c| c.is_ascii_digit()).count()
}

/// `(\d+)\.(\d+)(?:\.\d+)?-CL-(\d+)` anchored at i
fn release_at(b: &[u8], i: usize) -> Option<(f64, f64, f64)> {
    let num = |s: &[u8]| std::str::from_utf8(s).ok().and_then(|s| s.parse::<f64>().ok());
    let d1 = digits(b, i);
    if d1 == 0 || b.get(i + d1) != Some(&b'.') {
        return None;
    }
    let j = i + d1 + 1;
    let d2 = digits(b, j);
    if d2 == 0 {
        return None;
    }
    let mut k = j + d2;
    if b.get(k) == Some(&b'.') {
        let d3 = digits(b, k + 1);
        if d3 > 0 && b[k + 1 + d3..].starts_with(b"-CL-") {
            k += 1 + d3;
        }
    }
    if !b[k..].starts_with(b"-CL-") {
        return None;
    }
    let d4 = digits(b, k + 4);
    if d4 == 0 {
        return None;
    }
    Some((num(&b[i..i + d1])?, num(&b[j..j + d2])?, num(&b[k + 4..k + 4 + d4])?))
}

#[derive(Clone, Debug, Default)]
pub struct Wanted {
    pub playlist: String,
    pub seq: i64,
    pub at: i64,
}

/// `gameServer`: what the launcher posted to /offline/server-ready (JSON null when there is none).
pub struct GameServer {
    pub v: Value,
    pub ready_at: Option<i64>,
}

pub struct App {
    pub build: String,
    pub off: PathBuf,
    pub port: f64,
    pub account_id: String,
    pub display_name: String,
    pub server_id: String,
    pub no_match: bool,
    /// cosmetics.items (custom items merged): (templateId, lower-case id)
    pub cos_items: Vec<(String, String)>,
    /// `cosmetics.source || 'none'`
    pub cos_source: Value,
    pub custom: Value,
    pub variants_of: HashMap<String, Value>,
    pub hotfix: Vec<(String, String)>,
    pub state: RefCell<Obj>,
    pub tokens: RefCell<HashMap<String, String>>,
    pub game_server: RefCell<GameServer>,
    pub wanted: RefCell<Wanted>,
    pub build_unique_id: RefCell<String>,
    unknown: RefCell<HashSet<String>>,
    routes: Vec<Route>,
}

/// readJson: parsed file (one leading BOM ignored), None on any error.
pub fn read_json(f: &Path) -> Option<Value> {
    let b = std::fs::read(f).ok()?;
    let s = String::from_utf8_lossy(&b);
    js::parse(s.strip_prefix('\u{feff}').unwrap_or(&s))
}

/// fs.readdirSync: Node sorts the names on Unix (libuv scandir) and keeps the file system's order on Windows.
pub fn read_dir_names(d: &Path) -> std::io::Result<Vec<String>> {
    let mut names = Vec::new();
    for e in std::fs::read_dir(d)? {
        names.push(e?.file_name().to_string_lossy().into_owned());
    }
    if !cfg!(windows) {
        names.sort();
    }
    Ok(names)
}

/// Node-like text for a file system error ("ENOENT: no such file or directory, open '...'")
pub fn fs_error(e: &std::io::Error, syscall: &str, p: &Path) -> JsError {
    use std::io::ErrorKind::*;
    let (code, desc) = match e.kind() {
        NotFound => ("ENOENT", "no such file or directory"),
        PermissionDenied => ("EPERM", "operation not permitted"),
        AlreadyExists => ("EEXIST", "file already exists"),
        _ => {
            if p.is_dir() {
                return JsError { name: "Error", message: "EISDIR: illegal operation on a directory, read".into() };
            }
            return JsError { name: "Error", message: format!("{}, {} '{}'", e, syscall, p.display()) };
        }
    };
    JsError { name: "Error", message: format!("{}: {}, {} '{}'", code, desc, syscall, p.display()) }
}

/// fs.readFileSync
pub fn read_file(p: &Path) -> JsResult<Vec<u8>> {
    if p.is_dir() {
        return Err(JsError { name: "Error", message: "EISDIR: illegal operation on a directory, read".into() });
    }
    std::fs::read(p).map_err(|e| fs_error(&e, "open", p))
}

fn crash(e: JsError) -> (i32, String) {
    (1, format!("{}: {}", e.name, e.message))
}

fn is_hex32(s: &str) -> bool {
    s.len() == 32 && s.bytes().all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(&c))
}

impl App {
    /// Everything server.mjs does before listening. Err = (exit code, stderr text).
    pub fn start(args: &[String]) -> Result<App, (i32, String)> {
        // arg(n): undefined when absent, argv[i+1] (maybe undefined) when present
        let arg = |n: &str| -> Option<Option<String>> {
            let i = args.iter().position(|a| a == n).filter(|&i| i > 0)?;
            Some(args.get(i + 1).cloned())
        };
        let build = arg("--build").flatten().unwrap_or_default();
        let config_path = arg("--config").flatten().unwrap_or_default();
        if build.is_empty() || config_path.is_empty() {
            return Err((2, "usage: radarnite-backend --build <dir> --config <config.json>".into()));
        }
        let off = Path::new(&build).join("offline");
        std::fs::create_dir_all(&off).map_err(|e| crash(fs_error(&e, "mkdir", &off)))?;

        let config = read_json(Path::new(&config_path)).unwrap_or_else(|| json!({}));
        let cfg_port = js::prop(&config, "port").map_err(crash)?;
        let port_v = match arg("--port") {
            Some(v) => v.map(Value::String),
            None => Some(js::or(cfg_port, json!(3551))),
        };
        let port = js::to_number(port_v.as_ref());
        let acc = js::prop(&config, "accountId").map_err(crash)?;
        let account_id = match &acc {
            Some(Value::String(s)) if is_hex32(s) => s.clone(),
            _ => js::random_hex(16),
        };
        let dn = js::prop(&config, "displayName").map_err(crash)?;
        let display_name = js::slice_utf16(&js::to_string(Some(&js::or(dn, json!("OfflinePlayer")))), 32);

        let cosmetics = match read_json(&off.join("cosmetics.json")).unwrap_or_else(|| json!({ "items": [] })) {
            Value::Object(m) => m,
            Value::Array(_) => Obj::new(),
            Value::Null => return Err(crash(type_error("Cannot read properties of null (reading 'items')"))),
            other => {
                return Err(crash(type_error(format!(
                    "Cannot create property 'items' on {} '{}'",
                    js::type_name(&other),
                    js::to_string(Some(&other))
                ))))
            }
        };
        let custom = read_json(&off.join("custom-cosmetics.json")).unwrap_or_else(|| json!({ "items": [], "keychain": [] }));
        let mut items = js::nullish_or(cosmetics.get("items").cloned(), json!([]));
        // own cosmetics (custom-cosmetics.json) on top of the build's list, once per templateId
        let citems = js::prop(&custom, "items").map_err(crash)?;
        let list = if js::truthy(citems.as_ref()) { js::spread(citems.as_ref(), "customCosmetics.items").map_err(crash)? } else { vec![] };
        for it in list {
            let tid = js::opt_chain(Some(&it), "templateId");
            if !js::truthy(tid.as_ref()) {
                continue;
            }
            let Value::Array(arr) = &mut items else {
                return Err(crash(type_error("cosmetics.items.some is not a function")));
            };
            let mut found = false;
            for c in arr.iter() {
                let ct = js::prop(c, "templateId").map_err(crash)?;
                let cl = js::lower(ct.as_ref(), "c.templateId").map_err(crash)?;
                if cl == js::lower(tid.as_ref(), "it.templateId").map_err(crash)? {
                    found = true;
                    break;
                }
            }
            if !found {
                let mut o = Obj::new();
                o.insert("variants".into(), json!([]));
                js::spread_into(&mut o, &it);
                arr.push(Value::Object(o));
            }
        }
        let mut cos_items = Vec::new();
        let mut variants_of = HashMap::new();
        match &items {
            Value::Array(arr) => {
                for it in arr {
                    let tid = js::prop(it, "templateId").map_err(crash)?;
                    let id = js::lower(tid.as_ref(), "it.templateId").map_err(crash)?;
                    variants_of.insert(id.clone(), js::or(js::get(it, "variants"), json!([])));
                    cos_items.push((tid.and_then(|t| t.as_str().map(String::from)).unwrap_or_default(), id));
                }
            }
            v if !js::truthy(Some(v)) => {}
            Value::String(_) => return Err(crash(type_error("Cannot read properties of undefined (reading 'toLowerCase')"))),
            _ => return Err(crash(type_error("cosmetics.items is not iterable"))),
        }
        let cos_source = js::or(cosmetics.get("source").cloned(), json!("none"));

        let state_v = read_json(&off.join("state.json")).unwrap_or_else(|| json!({}));
        let mut state = match state_v {
            Value::Object(m) => m,
            Value::Array(_) => Obj::new(),
            Value::Null => return Err(crash(type_error("Cannot read properties of null (reading 'slots')"))),
            other => {
                return Err(crash(type_error(format!(
                    "Cannot create property 'slots' on {} '{}'",
                    js::type_name(&other),
                    js::to_string(Some(&other))
                ))))
            }
        };
        // Character, Backpack, Pickaxe, Glider, SkyDiveContrail, MusicPack, LoadingScreen, Dance[6], ItemWrap[7]
        for (k, d) in [
            ("slots", json!({})),
            ("favorites", json!({})),
            ("seen", json!({})),
            ("banner", json!({ "icon": "BRSeason01", "color": "DefaultColor1" })),
            ("lockerName", json!("")),
            ("rvn", json!({})),
        ] {
            if state.get(k).map_or(true, Value::is_null) {
                js::set(&mut state, k, d);
            }
        }

        let mut app = App {
            build,
            off,
            port,
            server_id: js::md5_hex(format!("server{}", account_id).as_bytes()),
            account_id,
            display_name,
            no_match: args.iter().any(|a| a == "--no-match"),
            cos_items,
            cos_source,
            custom,
            variants_of,
            hotfix: Vec::new(),
            state: RefCell::new(state),
            tokens: RefCell::new(HashMap::new()),
            game_server: RefCell::new(GameServer { v: Value::Null, ready_at: None }),
            wanted: RefCell::new(Wanted::default()),
            build_unique_id: RefCell::new("0".into()),
            unknown: RefCell::new(HashSet::new()),
            routes: routes::table(),
        };
        app.load_hotfix();
        if !(port.fract() == 0.0 && (0.0..=65535.0).contains(&port)) {
            return Err((
                1,
                format!(
                    "RangeError [ERR_SOCKET_BAD_PORT]: options.port should be >= 0 and < 65536. Received type number ({}).",
                    js::num_to_string(port)
                ),
            ));
        }
        Ok(app)
    }

    /// Hotfix files (Epic's "cloudstorage system" files: ini overrides the game applies at login). They point the
    /// game's XMPP at this backend and turn off the content beacon; <build>/offline/hotfix/*.ini are appended.
    fn load_hotfix(&mut self) {
        let engine = [
            "[OnlineSubsystemMcp.Xmpp]".to_string(),
            "bUseSSL=false".into(),
            format!("ServerPort={}", js::num_to_string(self.port)),
            "".into(),
            "[OnlineSubsystemMcp.Xmpp Prod]".into(),
            "ServerAddr=\"ws://127.0.0.1\"".into(),
            "Domain=prod.ol.epicgames.com".into(),
            "".into(),
            "[ConsoleVariables]".into(),
            "FortMatchmakingV2.EnableContentBeacon=0".into(),
            "FortMatchmakingV2.ContentBeaconFailureCancelsMatchmaking=0".into(),
            "Fort.ShutdownWhenContentBeaconFails=0".into(),
            "".into(),
        ]
        .join("\r\n");
        self.hotfix.push(("DefaultEngine.ini".into(), engine));
        let hd = self.off.join("hotfix");
        let r = (|| -> JsResult<()> {
            if !hd.exists() {
                return Ok(());
            }
            let names = read_dir_names(&hd).map_err(|e| fs_error(&e, "scandir", &hd))?;
            for f in names.into_iter().filter(|x| x.to_lowercase().ends_with(".ini")) {
                let b = read_file(&hd.join(&f))?;
                let s = String::from_utf8_lossy(&b);
                let s = s.strip_prefix('\u{feff}').unwrap_or(&s);
                match self.hotfix.iter().position(|e| e.0 == f) {
                    Some(i) => {
                        let v = &mut self.hotfix[i].1;
                        if !v.is_empty() {
                            v.push_str("\r\n");
                        }
                        v.push_str(s);
                    }
                    None => self.hotfix.push((f, s.to_string())),
                }
            }
            Ok(())
        })();
        if let Err(e) = r {
            self.log(&format!("hotfix dir {}", e.message));
        }
    }

    /// One line in <build>/offline/backend.log
    pub fn log(&self, line: &str) {
        let _ = std::fs::OpenOptions::new()
            .create(true)
            .append(true)
            .open(self.off.join("backend.log"))
            .and_then(|mut f| f.write_all(format!("{} {}\n", js::now_iso(), line).as_bytes()));
    }

    /// state.json, written atomically (temp file + rename; a plain write if the rename is refused)
    pub fn save_state(&self, st: &Obj) {
        let s = js::stringify_obj(st);
        let dst = self.off.join("state.json");
        let tmp = self.off.join("state.json.tmp");
        if std::fs::write(&tmp, &s).and_then(|_| std::fs::rename(&tmp, &dst)).is_err() {
            let _ = std::fs::remove_file(&tmp);
            if let Err(e) = std::fs::write(&dst, &s) {
                self.log(&format!("state save failed {}", fs_error(&e, "open", &dst).message));
            }
        }
    }

    pub fn token(&self, login: &str) -> Value {
        let server = login.len() >= 7 && login.as_bytes()[..7].eq_ignore_ascii_case(b"server@");
        let (id, name) = if server { (&self.server_id, "OfflineServer") } else { (&self.account_id, self.display_name.as_str()) };
        let t = format!("eg1~offline-{}", js::random_hex(16));
        self.tokens.borrow_mut().insert(t.clone(), id.clone());
        json!({ "access_token": t, "expires_in": 28800, "expires_at": FAR, "token_type": "bearer",
            "refresh_token": format!("offline-refresh-{}", js::random_hex(8)), "refresh_expires": 86400, "refresh_expires_at": FAR,
            "account_id": id, "client_id": "ec684b8c687f479fadea3cb2ad83f5c6", "internal_client": true, "client_service": "fortnite",
            "displayName": name, "app": "fortnite", "in_app_id": id, "device_id": "offline-device", "product_id": "prod-fn",
            "application_id": "fghi4567FNFBKFz3E4TROb0bmPS8h1GW" })
    }

    pub fn account(&self) -> Obj {
        js::obj(json!({ "id": self.account_id, "displayName": self.display_name, "name": self.display_name,
            "email": format!("{}@offline.local", self.account_id), "failedLoginAttempts": 0, "lastLogin": js::now_iso(),
            "numberOfDisplayNameChanges": 0, "ageGroup": "UNKNOWN", "headless": false, "country": "ES", "lastName": "Offline",
            "preferredLanguage": "en", "canUpdateDisplayName": false, "tfaEnabled": false, "emailVerified": true,
            "minorVerified": false, "minorExpected": false, "minorStatus": "NOT_MINOR", "cabinedMode": false, "hasHashedEmail": false }))
    }

    pub fn public_account(&self) -> Value {
        json!({ "id": self.account_id, "displayName": self.display_name, "externalAuths": {} })
    }

    /// serverFor(w): the match server, if it serves w's playlist and became ready after w was asked for
    pub fn server_for(&self, w: &Wanted) -> Option<Value> {
        let g = self.game_server.borrow();
        if !js::truthy(Some(&g.v)) {
            return None;
        }
        let pl = js::get(&g.v, "playlist");
        if !js::strict_eq(pl.as_ref(), Some(&Value::String(w.playlist.clone()))) {
            return None;
        }
        if g.ready_at? >= w.at {
            Some(g.v.clone())
        } else {
            None
        }
    }

    /// The request loop body of server.mjs: first matching route (method, then case-insensitive path), a 500
    /// per route when it throws or panics, else an Epic-style 404 logged once in unknown-routes.log.
    pub fn dispatch(&self, method: String, raw_url: String, ua: String, auth: Option<String>, body: Vec<u8>) -> Resp {
        let Some((pathname, query)) = js::url_parse(&raw_url) else {
            // server.mjs crashes here (uncaught ERR_INVALID_URL); answered like its per-route catch instead
            self.log(&format!("ERR {} {} TypeError: Invalid URL", method, raw_url));
            return routes::json(500, &json!({ "errorCode": "errors.com.epicgames.offline.internal", "errorMessage": "Invalid URL" }));
        };
        let info = Info::from_ua(&ua);
        for r in &self.routes {
            if r.method != method {
                continue;
            }
            let Some(m) = r.matches(&pathname) else { continue };
            let q = Req { method, pathname, query, auth, body, m, info };
            let err = match catch_unwind(AssertUnwindSafe(|| (r.handler)(self, &q))) {
                Ok(Ok(resp)) => return resp,
                Ok(Err(e)) => e,
                Err(p) => JsError {
                    name: "Error",
                    message: p
                        .downcast_ref::<&str>()
                        .map(|s| s.to_string())
                        .or_else(|| p.downcast_ref::<String>().cloned())
                        .unwrap_or_else(|| "internal error".into()),
                },
            };
            self.log(&format!("ERR {} {} {}: {}", q.method, q.pathname, err.name, err.message));
            return routes::json(
                500,
                &json!({ "errorCode": "errors.com.epicgames.offline.internal", "errorMessage": err.message }),
            );
        }
        let key = format!("{} {}", method, pathname);
        if self.unknown.borrow_mut().insert(key.clone()) {
            let _ = std::fs::OpenOptions::new()
                .create(true)
                .append(true)
                .open(self.off.join("unknown-routes.log"))
                .and_then(|mut f| f.write_all(format!("{} {} {}\n", js::now_iso(), info.version, key).as_bytes()));
        }
        routes::not_found(&pathname)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn build_info_from_user_agent() {
        let i = Info::from_ua("Fortnite/++Fortnite+Release-17.50-CL-17388565 Windows/10.0.19045.1.256.64bit");
        assert_eq!((i.season, i.version.as_str()), (17.0, "17.50"));
        let i = Info::from_ua("Fortnite/++Fortnite+Release-3.6-CL-4019403 Windows/10");
        assert_eq!((i.season, i.version.as_str()), (3.0, "3.6"));
        let i = Info::from_ua("Fortnite/++Fortnite+Release-1.8.2-CL-3724489 Windows");
        assert_eq!((i.season, i.version.as_str()), (1.0, "1.8"));
        let i = Info::from_ua("Release-1.10-CL-3790078 Release-x");
        assert_eq!(i.season, 2.0);
        let i = Info::from_ua("Release-x Release-07.40-CL-1");
        assert_eq!(i.version, "7.40");
        assert_eq!(Info::from_ua("curl/8").version, "?");
        assert_eq!(Info::from_ua("Release-1.2.3x-CL-5").version, "?");
    }
}
