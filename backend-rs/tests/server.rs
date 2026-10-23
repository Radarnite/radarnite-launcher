//! End-to-end tests: the real binary against a temporary build folder, spoken to over plain TCP.

use std::io::{BufRead, BufReader, Read, Write};
use std::net::{TcpListener, TcpStream};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicU32, Ordering};
use std::time::{Duration, Instant};

const ACC: &str = "0123456789abcdef0123456789abcdef";
const UA: &str = "Fortnite/++Fortnite+Release-17.50-CL-17388565 Windows/10.0.19045.1.256.64bit";
static N: AtomicU32 = AtomicU32::new(0);

struct Srv {
    child: Child,
    port: u16,
    dir: PathBuf,
}

impl Drop for Srv {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
        let _ = std::fs::remove_dir_all(&self.dir);
    }
}

impl Srv {
    fn off(&self) -> PathBuf {
        self.dir.join("build").join("offline")
    }
    fn get(&self, path: &str) -> Res {
        http(self.port, "GET", path, &[], b"")
    }
    fn post(&self, path: &str, body: &str) -> Res {
        http(self.port, "POST", path, &[], body.as_bytes())
    }
}

fn temp_dir() -> PathBuf {
    let d = std::env::temp_dir().join(format!("rb-test-{}-{}", std::process::id(), N.fetch_add(1, Ordering::SeqCst)));
    let _ = std::fs::remove_dir_all(&d);
    std::fs::create_dir_all(d.join("build").join("offline")).unwrap();
    d
}

fn free_port() -> u16 {
    TcpListener::bind("127.0.0.1:0").unwrap().local_addr().unwrap().port()
}

fn spawn(dir: &Path, port: u16, extra: &[&str]) -> Child {
    let mut args = vec![
        "--build".to_string(),
        dir.join("build").to_string_lossy().into_owned(),
        "--config".into(),
        dir.join("config.json").to_string_lossy().into_owned(),
        "--port".into(),
        port.to_string(),
    ];
    args.extend(extra.iter().map(|s| s.to_string()));
    Command::new(env!("CARGO_BIN_EXE_radarnite-backend"))
        .args(&args)
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .unwrap()
}

fn start_with(setup: impl FnOnce(&Path), extra: &[&str]) -> Srv {
    let dir = temp_dir();
    std::fs::write(dir.join("config.json"), format!(r#"{{"displayName":"Tester","accountId":"{}","_help":"x"}}"#, ACC)).unwrap();
    std::fs::write(
        dir.join("build/offline/cosmetics.json"),
        r#"{"source":"test","items":[{"templateId":"AthenaCharacter:CID_A_085_Athena_Commando_F_Hardwood_K7ZZ1","variants":[{"channel":"Material","active":"Mat1","owned":["Mat1","Mat2"]}]},{"templateId":"AthenaPickaxe:Pickaxe_Lockjaw","variants":[]}]}"#,
    )
    .unwrap();
    setup(&dir);
    let port = free_port();
    let mut child = spawn(&dir, port, extra);
    // wait for the ready line before connecting
    let mut line = String::new();
    BufReader::new(child.stdout.take().unwrap()).read_line(&mut line).unwrap();
    Srv { child, port, dir }
}

fn start() -> Srv {
    start_with(|_| {}, &[])
}

struct Res {
    status: u16,
    body: String,
}

fn http(port: u16, method: &str, path: &str, headers: &[(&str, &str)], body: &[u8]) -> Res {
    let mut s = TcpStream::connect(("127.0.0.1", port)).unwrap();
    s.set_read_timeout(Some(Duration::from_secs(10))).unwrap();
    let mut req = format!("{} {} HTTP/1.1\r\nHost: 127.0.0.1\r\nUser-Agent: {}\r\nConnection: close\r\nContent-Length: {}\r\n", method, path, UA, body.len());
    for (k, v) in headers {
        req.push_str(&format!("{}: {}\r\n", k, v));
    }
    req.push_str("\r\n");
    s.write_all(req.as_bytes()).unwrap();
    s.write_all(body).unwrap();
    let mut out = Vec::new();
    s.read_to_end(&mut out).unwrap();
    let text = String::from_utf8_lossy(&out).into_owned();
    let (head, body) = text.split_once("\r\n\r\n").unwrap();
    Res { status: head[9..12].parse().unwrap(), body: body.to_string() }
}

fn json(s: &str) -> serde_json::Value {
    serde_json::from_str(s).unwrap()
}

#[test]
fn locker_changes_go_to_state_json_keeping_unknown_keys() {
    let s = start_with(|d| std::fs::write(d.join("build/offline/state.json"), "\u{feff}{\"_mine\":[1,2],\"rvn\":{\"athena\":5}}").unwrap(), &[]);
    let q = json(&s.post(&format!("/fortnite/api/game/v2/profile/{}/client/QueryProfile?profileId=athena&rvn=-1", ACC), "{}").body);
    assert_eq!(q["profileRevision"], 5);
    let p = &q["profileChanges"][0]["profile"];
    assert_eq!(p["stats"]["attributes"]["season_num"], 17);
    let it = &p["items"]["athenacharacter:cid_a_085_athena_commando_f_hardwood_k7zz1"];
    assert_eq!(it["templateId"], "AthenaCharacter:CID_A_085_Athena_Commando_F_Hardwood_K7ZZ1");
    assert_eq!(it["attributes"]["variants"][0]["owned"], serde_json::json!(["Mat1", "Mat2"]));
    assert_eq!(p["items"]["athenapickaxe:defaultpickaxe"]["quantity"], 1);
    assert_eq!(p["items"]["sandbox_loadout"]["attributes"]["locker_slots_data"]["slots"]["Dance"]["items"][0], "AthenaDance:eid_dancemoves");
    let body = r#"{"lockerItem":"sandbox_loadout","category":"Character","itemToSlot":"AthenaCharacter:CID_A_085_Athena_Commando_F_Hardwood_K7ZZ1","slotIndex":0,"variantUpdates":[{"channel":"Material","active":"Mat3","owned":[]},{"channel":"Parts","active":"Stage2"}]}"#;
    let e = json(&s.post(&format!("/fortnite/api/game/v2/profile/{}/client/SetCosmeticLockerSlot?profileId=athena&rvn=-1", ACC), body).body);
    assert_eq!(e["profileRevision"], 6);
    let st = std::fs::read_to_string(s.off().join("state.json")).unwrap();
    assert_eq!(
        st,
        r#"{"_mine":[1,2],"rvn":{"athena":6},"slots":{"Character":["AthenaCharacter:CID_A_085_Athena_Commando_F_Hardwood_K7ZZ1"]},"favorites":{},"seen":{},"banner":{"icon":"BRSeason01","color":"DefaultColor1"},"lockerName":"","variantsActive":{"athenacharacter:cid_a_085_athena_commando_f_hardwood_k7zz1":[{"channel":"Material","active":"Mat3","owned":["Mat1","Mat2","Mat3"]},{"channel":"Parts","active":"Stage2","owned":["Stage2"]}]},"slotVariants":{"Character":[{"variants":[{"channel":"Material","active":"Mat3"},{"channel":"Parts","active":"Stage2"}]}]}}"#
    );
    assert!(!s.off().join("state.json.tmp").exists());
    let log = std::fs::read_to_string(s.off().join("backend.log")).unwrap();
    assert!(log.contains("variants AthenaCharacter:CID_A_085_Athena_Commando_F_Hardwood_K7ZZ1: Material=Mat3, Parts=Stage2"), "{}", log);
    // favorites, banner, a dance in slot 3, and a command that changes nothing
    s.post(&format!("/fortnite/api/game/v2/profile/{}/client/SetItemFavoriteStatusBatch?profileId=athena", ACC),
        r#"{"itemIds":["AthenaPickaxe:Pickaxe_Lockjaw"],"itemFavStatus":[true]}"#);
    s.post(&format!("/fortnite/api/game/v2/profile/{}/client/SetBattleRoyaleBanner?profileId=athena", ACC), r#"{"bannerIconTemplateName":"OtherBanner"}"#);
    let d = json(&s.post(&format!("/fortnite/api/game/v2/profile/{}/client/EquipBattleRoyaleCustomization?profileId=athena", ACC),
        r#"{"slotName":"Dance","itemToSlot":"AthenaDance:EID_Floss","indexWithinSlot":3}"#).body);
    assert_eq!(d["profileRevision"], 9);
    let a = &d["profileChanges"][0]["profile"];
    assert_eq!(a["items"]["athenapickaxe:pickaxe_lockjaw"]["attributes"]["favorite"], true);
    assert_eq!(a["stats"]["attributes"]["banner_icon"], "OtherBanner");
    assert_eq!(a["stats"]["attributes"]["favorite_dance"],
        serde_json::json!(["AthenaDance:eid_dancemoves", "", "", "AthenaDance:EID_Floss", "", ""]));
    let c = json(&s.post(&format!("/fortnite/api/game/v2/profile/{}/client/QueryProfile?profileId=common_core", ACC), "").body);
    assert_eq!(c["profileRevision"], 1);
    assert_eq!(c["profileId"], "common_core");
    let m = json(&s.post(&format!("/fortnite/api/game/v2/profile/{}/client/MarkItemSeen?profileId=athena", ACC), "not json").body);
    assert_eq!(m["profileRevision"], 9);
    // a throwing command: 500 with the JS error, the server keeps going
    let r = s.post(&format!("/fortnite/api/game/v2/profile/{}/client/SetCosmeticLockerSlot?profileId=athena", ACC), "null");
    assert_eq!(r.status, 500);
    assert_eq!(r.body, r#"{"errorCode":"errors.com.epicgames.offline.internal","errorMessage":"Cannot read properties of null (reading 'category')"}"#);
    assert_eq!(s.get("/offline/health").status, 200);
}

#[test]
fn bad_uri_is_a_500_like_node() {
    let s = start();
    let r = s.get("/fortnite/api/cloudstorage/system/%E0%A4%A");
    assert_eq!(r.status, 500);
    assert_eq!(r.body, r#"{"errorCode":"errors.com.epicgames.offline.internal","errorMessage":"URI malformed"}"#);
    // `new URL()` throws on these in server.mjs (the process dies); here: the per-route 500
    let r = s.get("//[x/offline/health");
    assert_eq!((r.status, r.body.as_str()), (500, r#"{"errorCode":"errors.com.epicgames.offline.internal","errorMessage":"Invalid URL"}"#));
    assert_eq!(s.get("//host/offline/health").status, 200);
    let r = s.get("/fortnite/api/cloudstorage/system/constructor");
    assert_eq!(r.status, 500);
    let log = std::fs::read_to_string(s.off().join("backend.log")).unwrap();
    assert!(log.contains("ERR GET /fortnite/api/cloudstorage/system/%E0%A4%A URIError: URI malformed"), "{}", log);
}

#[test]
fn cloud_storage_round_trip() {
    let s = start();
    assert_eq!(s.get(&format!("/fortnite/api/cloudstorage/user/{}/ClientSettings.Sav", ACC)).status, 204);
    assert_eq!(http(s.port, "PUT", &format!("/fortnite/api/cloudstorage/user/{}/ClientSettings.Sav", ACC), &[], b"\x00\x01save").status, 204);
    let r = s.get(&format!("/fortnite/api/cloudstorage/user/{}/ClientSettings.Sav", ACC));
    assert_eq!((r.status, r.body.as_bytes()), (200, &b"\x00\x01save"[..]));
    let l = json(&s.get(&format!("/fortnite/api/cloudstorage/user/{}", ACC)).body);
    assert_eq!(l[0]["filename"], "ClientSettings.Sav");
    assert_eq!(l[0]["length"], 6);
    assert_eq!(l[0]["hash"].as_str().unwrap().len(), 40);
    assert_eq!(l[0]["hash256"].as_str().unwrap().len(), 64);
    assert_eq!(l[0]["accountId"], ACC);
}

struct WsClient {
    s: TcpStream,
    buf: Vec<u8>,
}

impl WsClient {
    fn open(port: u16, path: &str, protocol: Option<&str>) -> (WsClient, String) {
        let mut s = TcpStream::connect(("127.0.0.1", port)).unwrap();
        s.set_read_timeout(Some(Duration::from_secs(5))).unwrap();
        let p = protocol.map(|p| format!("Sec-WebSocket-Protocol: {}\r\n", p)).unwrap_or_default();
        write!(s, "GET {} HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n{}\r\n", path, p).unwrap();
        let mut c = WsClient { s, buf: Vec::new() };
        let head = loop {
            if let Some(i) = c.buf.windows(4).position(|w| w == b"\r\n\r\n") {
                let h = String::from_utf8_lossy(&c.buf[..i]).into_owned();
                c.buf.drain(..i + 4);
                break h;
            }
            if c.fill() == 0 {
                break String::new();
            }
        };
        (c, head)
    }
    fn fill(&mut self) -> usize {
        let mut t = [0u8; 4096];
        let n = self.s.read(&mut t).unwrap_or(0);
        self.buf.extend_from_slice(&t[..n]);
        n
    }
    fn frame(&mut self) -> Option<(u8, Vec<u8>)> {
        loop {
            if self.buf.len() >= 2 {
                let short = (self.buf[1] & 127) as usize;
                let header = if short == 126 { 4 } else { 2 };
                if self.buf.len() >= header {
                    let len = if short == 126 { u16::from_be_bytes([self.buf[2], self.buf[3]]) as usize } else { short };
                    if self.buf.len() >= header + len {
                        let op = self.buf[0] & 15;
                        let p = self.buf[header..header + len].to_vec();
                        self.buf.drain(..header + len);
                        return Some((op, p));
                    }
                }
            }
            if self.fill() == 0 {
                return None;
            }
        }
    }
    fn text(&mut self) -> String {
        let (op, p) = self.frame().expect("a frame");
        assert_eq!(op, 1);
        String::from_utf8(p).unwrap()
    }
    fn send(&mut self, op: u8, p: &[u8]) {
        let mask = [1u8, 2, 3, 4];
        let mut f = vec![0x80 | op];
        if p.len() < 126 {
            f.push(0x80 | p.len() as u8);
        } else {
            f.push(0x80 | 126);
            f.extend_from_slice(&(p.len() as u16).to_be_bytes());
        }
        f.extend_from_slice(&mask);
        f.extend(p.iter().enumerate().map(|(i, b)| b ^ mask[i & 3]));
        self.s.write_all(&f).unwrap();
    }
}

#[test]
fn matchmaker_waits_for_the_match_server_then_plays() {
    let s = start();
    let t = json(&s.get("/fortnite/api/game/v2/matchmakingservice/ticket/player/x?partyPlayerIds=x&bucketId=17388565%3A1%3AEU%3APlaylist_DefaultDuo%3APC&player.platform=Windows").body);
    assert_eq!(t["serviceUrl"], format!("ws://127.0.0.1:{}/matchmaking", s.port));
    let w = json(&s.get("/offline/wanted").body);
    assert_eq!((w["playlist"].as_str().unwrap(), w["seq"].as_i64().unwrap()), ("playlist_defaultduo", 1));
    assert!(w["server"].is_null());
    let t0 = Instant::now();
    let (mut c, head) = WsClient::open(s.port, "/matchmaking", None);
    // wsAccept's bytes exactly: no Date, Node's header case
    assert_eq!(head, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    assert_eq!(c.text(), r#"{"payload":{"state":"Connecting"},"name":"StatusUpdate"}"#);
    assert!(t0.elapsed() >= Duration::from_millis(150));
    assert_eq!(c.text(), r#"{"payload":{"totalPlayers":1,"connectedPlayers":1,"state":"Waiting"},"name":"StatusUpdate"}"#);
    let q = json(&c.text());
    assert_eq!(q["payload"]["state"], "Queued");
    assert_eq!(q["payload"]["estimatedWaitSec"], 89);
    assert!(t0.elapsed() >= Duration::from_millis(900));
    c.send(9, b"hi");
    assert_eq!(c.frame().unwrap(), (10, b"hi".to_vec()));
    // a server for another playlist does not count; the wanted one does
    assert_eq!(s.post("/offline/server-ready", r#"{"address":"x","port":7777,"playlist":"playlist_defaultsolo"}"#).status, 204);
    assert_eq!(json(&c.text())["payload"]["state"], "Queued");
    s.post("/offline/server-ready", r#"{"address":"127.0.0.1","port":7777,"playlist":"playlist_defaultduo"}"#);
    let a = json(&c.text());
    assert_eq!(a["payload"]["state"], "SessionAssignment");
    let ta = Instant::now();
    let p = json(&c.text());
    assert_eq!(p["name"], "Play");
    assert_eq!(p["payload"]["matchId"], a["payload"]["matchId"]);
    assert!(ta.elapsed() >= Duration::from_millis(300));
    assert_eq!(c.frame().unwrap(), (8, vec![0x03, 0xE8]));
    assert!(ta.elapsed() >= Duration::from_millis(800));
    let sess = json(&s.get("/fortnite/api/matchmaking/session/abc").body);
    assert_eq!(sess["buildUniqueId"], "17388565");
    assert_eq!(sess["serverAddress"], "127.0.0.1");
    assert_eq!(sess["serverPort"], 7777);
    assert_eq!(sess["attributes"]["PLAYLISTNAME_s"], "playlist_defaultduo");
    let w = json(&s.get("/offline/wanted").body);
    let keys: Vec<&String> = w["server"].as_object().unwrap().keys().collect();
    assert_eq!(keys, ["address", "port", "playlist", "readyAt"]);
    assert_eq!(s.post("/offline/server-down", "").status, 204);
    assert!(json(&s.get("/offline/wanted").body)["server"].is_null());
    let log = std::fs::read_to_string(s.off().join("backend.log")).unwrap();
    assert!(log.contains("matchmaker: wanted playlist_defaultduo #1"));
    assert!(log.contains("matchmaker: client sent to {\"address\":\"127.0.0.1\",\"port\":7777,\"playlist\":\"playlist_defaultduo\",\"readyAt\":"));
    assert!(log.contains("game server down"));
}

#[test]
fn xmpp_login_bind_presence() {
    let s = start();
    let (mut c, head) = WsClient::open(s.port, "/", Some("xmpp"));
    assert!(head.ends_with("\r\nSec-WebSocket-Protocol: xmpp"), "{}", head);
    c.send(1, b"<open xmlns=\"urn:ietf:params:xml:ns:xmpp-framing\" to=\"prod.ol.epicgames.com\" version=\"1.0\"/>");
    assert!(c.text().starts_with("<open xmlns=\"urn:ietf:params:xml:ns:xmpp-framing\" from=\"prod.ol.epicgames.com\" id=\""));
    assert!(c.text().contains("<mechanism>PLAIN</mechanism>"));
    let plain = format!("\0{}\0token", ACC);
    let b64 = base64(plain.as_bytes());
    c.send(1, format!("<auth mechanism=\"PLAIN\" xmlns=\"urn:ietf:params:xml:ns:xmpp-sasl\">{}</auth>", b64).as_bytes());
    assert_eq!(c.text(), "<success xmlns=\"urn:ietf:params:xml:ns:xmpp-sasl\"/>");
    c.send(1, b"<open/>");
    c.text();
    assert!(c.text().contains("xmpp-bind"));
    c.send(1, b"<iq id=\"_xmpp_bind1\" type=\"set\"><bind xmlns=\"urn:ietf:params:xml:ns:xmpp-bind\"><resource>V2:Fortnite:WIN::ABC</resource></bind></iq>");
    let jid = format!("{}@prod.ol.epicgames.com/V2:Fortnite:WIN::ABC", ACC);
    assert_eq!(c.text(), format!("<iq type=\"result\" id=\"_xmpp_bind1\" to=\"{0}\" xmlns=\"jabber:client\"><bind xmlns=\"urn:ietf:params:xml:ns:xmpp-bind\"><jid>{0}</jid></bind></iq>", jid));
    c.send(1, b"<iq id='s1' type='set'><session/></iq>");
    assert_eq!(c.text(), format!("<iq type=\"result\" id=\"s1\" to=\"{}\" from=\"prod.ol.epicgames.com\" xmlns=\"jabber:client\"/>", jid));
    c.send(1, b"<presence><status>{}</status></presence>");
    assert_eq!(c.text(), format!("<presence from=\"{}\" to=\"{}@prod.ol.epicgames.com\"><status>{{}}</status></presence>", jid, ACC));
    c.send(1, b"<message to='x'><body>hi</body></message>");
    c.send(1, b"<close xmlns=\"urn:ietf:params:xml:ns:xmpp-framing\"/>");
    assert_eq!(c.text(), "<close xmlns=\"urn:ietf:params:xml:ns:xmpp-framing\"/>");
    assert_eq!(c.frame().unwrap(), (8, vec![]));
    let log = std::fs::read_to_string(s.off().join("backend.log")).unwrap();
    assert!(log.contains("xmpp: client connected"));
}

#[test]
fn matchmaker_keeps_sending_after_the_client_half_closes() {
    let s = start();
    s.get("/fortnite/api/game/v2/matchmakingservice/ticket/player/x?bucketId=1%3A1%3AEU%3Aplaylist_defaultsolo");
    let (mut c, head) = WsClient::open(s.port, "/matchmaking", None);
    assert!(head.starts_with("HTTP/1.1 101 "));
    c.s.shutdown(std::net::Shutdown::Write).unwrap();
    assert!(c.text().contains("Connecting"));
    assert!(c.text().contains("Waiting"));
    assert!(c.text().contains("Queued"));
    assert!(c.text().contains("Queued"));
}

#[test]
fn frames_sent_with_the_upgrade_request_are_dropped_like_node() {
    let s = start();
    let mut t = TcpStream::connect(("127.0.0.1", s.port)).unwrap();
    t.set_read_timeout(Some(Duration::from_secs(5))).unwrap();
    let mut first = b"GET /xmpp HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: a\r\n\r\n".to_vec();
    first.extend_from_slice(&[0x81, 0x85, 0, 0, 0, 0]);
    first.extend_from_slice(b"<open");
    t.write_all(&first).unwrap();
    let mut c = WsClient { s: t, buf: Vec::new() };
    while !c.buf.windows(4).any(|w| w == b"\r\n\r\n") {
        assert!(c.fill() > 0);
    }
    let i = c.buf.windows(4).position(|w| w == b"\r\n\r\n").unwrap();
    c.buf.drain(..i + 4);
    c.send(1, b"<open/>");
    assert!(c.text().starts_with("<open "));
    assert!(c.text().starts_with("<stream:features"));
    c.send(8, b"");
    assert_eq!(c.frame().unwrap(), (8, vec![]));
    assert!(c.frame().is_none());
}

fn base64(b: &[u8]) -> String {
    const T: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut s = String::new();
    for c in b.chunks(3) {
        let n = (c[0] as u32) << 16 | (*c.get(1).unwrap_or(&0) as u32) << 8 | *c.get(2).unwrap_or(&0) as u32;
        for i in 0..4 {
            s.push(if i <= c.len() { T[(n >> (18 - 6 * i)) as usize & 63] as char } else { '=' });
        }
    }
    s
}
