//! The minimal WebSocket of server.mjs (RFC 6455, lines 458-485), the matchmaker on it (521-551) and the minimal
//! XMPP over WebSocket (RFC 7395, 490-519). Frames are parsed exactly like the JS: no FIN handling, fragments and
//! binary frames dropped, pings answered, a close frame answered with an empty close and the socket destroyed.

use crate::app::{App, Wanted};
use crate::js;
use bytes::Bytes;
use http_body_util::Full;
use hyper::body::Incoming;
use hyper::{Request, Response};
use hyper_util::rt::TokioIo;
use serde_json::json;
use std::io;
use std::rc::Rc;
use std::time::Duration;
use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt};
use tokio::net::TcpStream;
use tokio::time::Instant;

const GUID: &str = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
const DOMAIN: &str = "prod.ol.epicgames.com";

/// The socket of an upgraded connection
pub trait Io: AsyncRead + AsyncWrite + Unpin {}
impl<T: AsyncRead + AsyncWrite + Unpin> Io for T {}
type BoxIo = Box<dyn Io>;

/// What server.mjs's upgrade handler looks at: the request target and the Sec-WebSocket-Protocol / -Key values
/// (repeated headers joined with ", " like Node)
pub struct Upgrade {
    pub url: String,
    pub proto: String,
    pub key: String,
}

pub enum Sniff {
    /// the connection's first request asks for an upgrade; n = bytes already received
    Upgrade(Upgrade, usize),
    Http,
    Closed,
}

fn joined<'a>(vals: impl Iterator<Item = &'a [u8]>) -> Option<String> {
    let v: Vec<String> = vals.map(js::latin1).collect();
    if v.is_empty() {
        None
    } else {
        Some(v.join(", "))
    }
}

fn has_token<'a>(mut vals: impl Iterator<Item = &'a [u8]>, token: &str) -> bool {
    vals.any(|v| js::latin1(v).split(',').any(|t| t.trim().eq_ignore_ascii_case(token)))
}

/// Peeks at a new connection's first request head (nothing is consumed): an upgrade request is answered here
/// with the exact bytes of server.mjs's wsAccept, anything else goes to hyper.
pub async fn sniff(s: &TcpStream) -> Sniff {
    let mut buf = vec![0u8; 16 * 1024];
    let deadline = Instant::now() + Duration::from_secs(5);
    loop {
        let n = match tokio::time::timeout_at(deadline, s.peek(&mut buf)).await {
            Ok(Ok(0)) | Ok(Err(_)) => return Sniff::Closed,
            Ok(Ok(n)) => n,
            Err(_) => return Sniff::Http,
        };
        let mut headers = [httparse::EMPTY_HEADER; 64];
        let mut req = httparse::Request::new(&mut headers);
        match req.parse(&buf[..n]) {
            Ok(httparse::Status::Complete(_)) => {
                let h = || req.headers.iter();
                let upgrade = req.method != Some("CONNECT")
                    && h().any(|x| x.name.eq_ignore_ascii_case("upgrade"))
                    && has_token(h().filter(|x| x.name.eq_ignore_ascii_case("connection")).map(|x| x.value), "upgrade");
                if !upgrade {
                    return Sniff::Http;
                }
                let of = |name: &str| joined(h().filter(|x| x.name.eq_ignore_ascii_case(name)).map(|x| x.value));
                let u = Upgrade {
                    url: req.path.unwrap_or("").to_string(),
                    proto: of("sec-websocket-protocol").unwrap_or_default(),
                    key: of("sec-websocket-key").unwrap_or_else(|| "undefined".into()),
                };
                return Sniff::Upgrade(u, n);
            }
            Ok(httparse::Status::Partial) if n < buf.len() => tokio::time::sleep(Duration::from_millis(2)).await,
            _ => return Sniff::Http,
        }
    }
}

/// XMPP (path /xmpp or protocol xmpp), the matchmaker (/matchmaking), else None: the socket is destroyed
fn kind_for(app: &App, u: &Upgrade) -> Option<Kind> {
    if u.url.starts_with("/xmpp") || u.proto.contains("xmpp") {
        return Some(Kind::Xmpp);
    }
    if !u.url.starts_with("/matchmaking") {
        return None;
    }
    Some(Kind::Match(Box::new(Matchmaking {
        ticket_id: js::random_hex(16),
        match_id: js::random_hex(16),
        session_id: js::random_hex(16),
        w: app.wanted.borrow().clone(),
        t0: Instant::now(),
    })))
}

fn accept_key(key: &str) -> String {
    js::base64(&js::sha1(format!("{}{}", key, GUID).as_bytes()))
}

async fn run(app: Rc<App>, io: BoxIo, kind: Kind) {
    let ws = Ws { io, buf: Vec::new(), ended: false, destroyed: false };
    match kind {
        Kind::Xmpp => xmpp_session(app, ws).await,
        Kind::Match(m) => match_session(app, ws, *m).await,
    }
}

/// server.on('upgrade') on the raw socket: the request head and whatever arrived with it are dropped (server.mjs
/// ignores the upgrade 'head'), then wsAccept's 101 byte for byte.
pub async fn raw_upgrade(app: Rc<App>, mut s: TcpStream, received: usize, u: Upgrade) {
    let mut sink = vec![0u8; received];
    if s.read_exact(&mut sink).await.is_err() {
        return;
    }
    let Some(kind) = kind_for(&app, &u) else { return };
    let xmpp = matches!(kind, Kind::Xmpp);
    let protocol = if xmpp && u.proto.contains("xmpp") { "Sec-WebSocket-Protocol: xmpp\r\n" } else { "" };
    let head = format!(
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: {}\r\n{}\r\n",
        accept_key(&u.key),
        protocol
    );
    if s.write_all(head.as_bytes()).await.is_err() {
        return;
    }
    if xmpp {
        app.log("xmpp: client connected");
    }
    let _ = s.set_nodelay(true);
    run(app, Box::new(s), kind).await;
}

/// An upgrade that is not the connection's first request goes through hyper (same routing; hyper writes the 101)
pub fn upgrade(app: Rc<App>, mut req: Request<Incoming>, raw_url: &str) -> Result<Response<Full<Bytes>>, io::Error> {
    let hv = |name: &str| joined(req.headers().get_all(name).iter().map(|v| v.as_bytes()));
    let u = Upgrade {
        url: raw_url.to_string(),
        proto: hv("sec-websocket-protocol").unwrap_or_default(),
        key: hv("sec-websocket-key").unwrap_or_else(|| "undefined".into()),
    };
    let Some(kind) = kind_for(&app, &u) else {
        return Err(io::Error::other("upgrade refused"));
    };
    let xmpp = matches!(kind, Kind::Xmpp);
    let mut res = Response::builder()
        .status(101)
        .header("Upgrade", "websocket")
        .header("Connection", "Upgrade")
        .header("Sec-WebSocket-Accept", accept_key(&u.key));
    if xmpp && u.proto.contains("xmpp") {
        res = res.header("Sec-WebSocket-Protocol", "xmpp");
    }
    let on_upgrade = hyper::upgrade::on(&mut req);
    let app2 = app.clone();
    tokio::task::spawn_local(async move {
        if let Ok(up) = on_upgrade.await {
            // bytes read past the request are dropped like server.mjs's upgrade 'head'
            let io: BoxIo = match up.downcast::<TokioIo<TcpStream>>() {
                Ok(parts) => Box::new(parts.io.into_inner()),
                Err(up) => Box::new(TokioIo::new(up)),
            };
            run(app2, io, kind).await;
        }
    });
    if xmpp {
        app.log("xmpp: client connected");
    }
    res.body(Full::new(Bytes::new())).map_err(io::Error::other)
}

enum Kind {
    Xmpp,
    Match(Box<Matchmaking>),
}

struct Matchmaking {
    ticket_id: String,
    match_id: String,
    session_id: String,
    w: Wanted,
    t0: Instant,
}

struct Ws<T> {
    io: T,
    buf: Vec<u8>,
    /// socket.end() was called (our side is closed: a later write destroys the socket, like Node's write-after-end)
    ended: bool,
    destroyed: bool,
}

enum Frame {
    Text(String),
    Close,
    Ping(Vec<u8>),
    Other,
}

impl<T: AsyncRead + AsyncWrite + Unpin> Ws<T> {
    async fn write(&mut self, data: &[u8]) {
        if self.destroyed {
            return;
        }
        if self.ended || self.io.write_all(data).await.is_err() {
            self.destroyed = true;
        }
    }

    /// ws.send(text)
    async fn send(&mut self, text: &str) {
        let p = text.as_bytes();
        let mut f = Vec::with_capacity(p.len() + 10);
        if p.len() < 126 {
            f.extend_from_slice(&[0x81, p.len() as u8]);
        } else if p.len() < 65536 {
            f.extend_from_slice(&[0x81, 126, (p.len() >> 8) as u8, p.len() as u8]);
        } else {
            f.extend_from_slice(&[0x81, 127]);
            f.extend_from_slice(&(p.len() as u64).to_be_bytes());
        }
        f.extend_from_slice(p);
        self.write(&f).await;
    }

    /// ws.close(code): code 1000 = normal closure; without a code the peer sees 1005
    async fn close(&mut self, code: Option<u16>) {
        let f = match code {
            Some(c) => vec![0x88, 2, (c >> 8) as u8, c as u8],
            None => vec![0x88, 0],
        };
        self.write(&f).await;
        if !self.destroyed {
            let _ = self.io.shutdown().await;
            self.ended = true;
        }
    }

    /// One frame out of the buffer, or None until more bytes arrive
    fn next_frame(&mut self) -> Option<Frame> {
        let b = &self.buf;
        if b.len() < 2 {
            return None;
        }
        let op = b[0] & 15;
        let masked = b[1] & 128 != 0;
        let mut len = (b[1] & 127) as u64;
        let mut off = 2usize;
        if len == 126 {
            if b.len() < 4 {
                return None;
            }
            len = u16::from_be_bytes([b[2], b[3]]) as u64;
            off = 4;
        } else if len == 127 {
            if b.len() < 10 {
                return None;
            }
            len = u64::from_be_bytes(b[2..10].try_into().unwrap());
            off = 10;
        }
        let mask_at = off;
        if masked {
            off += 4;
        }
        let end = (off as u64).checked_add(len)?;
        if (b.len() as u64) < end {
            return None;
        }
        let end = end as usize;
        let mut payload = b[off..end].to_vec();
        if masked {
            let mask = [b[mask_at], b[mask_at + 1], b[mask_at + 2], b[mask_at + 3]];
            for (i, x) in payload.iter_mut().enumerate() {
                *x ^= mask[i & 3];
            }
        }
        self.buf.drain(..end);
        Some(match op {
            1 => Frame::Text(String::from_utf8_lossy(&payload).into_owned()),
            8 => Frame::Close,
            9 => Frame::Ping(payload),
            _ => Frame::Other,
        })
    }

    /// Reads and handles frames until EOF; Some(text) for each text frame, None when the socket is gone.
    /// Close: answer and destroy. Ping: pong.
    async fn handle_frame(&mut self, f: Frame) -> Option<String> {
        match f {
            Frame::Text(t) => return Some(t),
            Frame::Close => {
                self.close(None).await;
                self.destroyed = true;
            }
            Frame::Ping(p) => {
                let mut f = vec![0x8A, p.len() as u8];
                f.extend_from_slice(&p);
                self.write(&f).await;
            }
            Frame::Other => {}
        }
        None
    }
}

fn status(payload: serde_json::Value, name: &str) -> String {
    js::stringify(&json!({ "payload": payload, "name": name }))
}

/// The matchmaker: Connecting (200 ms), Waiting (600 ms), then Queued every second until the match server for the
/// wanted playlist is ready (starting one takes 30-120 s); then SessionAssignment, Play 400 ms later and a normal
/// close (1000) at 900 ms: 17.x closes the socket itself after Play, 3.x waits for the service to close it.
async fn match_session(app: Rc<App>, mut ws: Ws<BoxIo>, m: Matchmaking) {
    let ms = |n: u64| Duration::from_millis(n);
    let mut steps = 0u32;
    let mut next_poll = Some(m.t0 + ms(1000));
    let mut play_at: Option<Instant> = None;
    let mut close_at: Option<Instant> = None;
    let mut tmp = vec![0u8; 16384];
    // http.Server sockets allow half-open: after the client's FIN the timers keep sending until a write fails
    let mut reading = true;
    loop {
        if ws.destroyed {
            return;
        }
        let step_at = if steps < 2 { Some(m.t0 + ms(200 + 400 * steps as u64)) } else { None };
        let deadline = [step_at, next_poll, play_at, close_at].into_iter().flatten().min();
        if !reading && (ws.ended || deadline.is_none()) {
            return;
        }
        tokio::select! {
            r = ws.io.read(&mut tmp), if reading => {
                match r {
                    Ok(0) => reading = false,
                    Err(_) => return,
                    Ok(n) => {
                        ws.buf.extend_from_slice(&tmp[..n]);
                        while let Some(f) = ws.next_frame() {
                            ws.handle_frame(f).await;
                            if ws.destroyed {
                                return;
                            }
                        }
                    }
                }
            }
            _ = tokio::time::sleep_until(deadline.unwrap_or_else(|| Instant::now() + ms(86_400_000))), if deadline.is_some() => {
                let now = Instant::now();
                if step_at.map_or(false, |t| t <= now) {
                    let payload = if steps == 0 {
                        json!({ "state": "Connecting" })
                    } else {
                        json!({ "totalPlayers": 1, "connectedPlayers": 1, "state": "Waiting" })
                    };
                    steps += 1;
                    ws.send(&status(payload, "StatusUpdate")).await;
                } else if next_poll.map_or(false, |t| t <= now) {
                    match app.server_for(&m.w) {
                        None => {
                            next_poll = next_poll.map(|t| t + ms(1000));
                            let waited = (m.t0.elapsed().as_millis() as f64 / 1000.0 + 0.5).floor();
                            let est = (90.0 - waited).max(5.0);
                            ws.send(&status(json!({ "ticketId": m.ticket_id, "queuedPlayers": 0, "estimatedWaitSec": js::num(est),
                                "status": {}, "state": "Queued" }), "StatusUpdate")).await;
                        }
                        Some(g) => {
                            next_poll = None;
                            ws.send(&status(json!({ "matchId": m.match_id, "state": "SessionAssignment" }), "StatusUpdate")).await;
                            play_at = Some(now + ms(400));
                            close_at = Some(now + ms(900));
                            app.log(&format!("matchmaker: client sent to {}", js::stringify(&g)));
                        }
                    }
                } else if play_at.map_or(false, |t| t <= now) {
                    play_at = None;
                    ws.send(&status(json!({ "matchId": m.match_id, "sessionId": m.session_id, "joinDelaySec": 1 }), "Play")).await;
                } else if close_at.map_or(false, |t| t <= now) {
                    close_at = None;
                    ws.close(Some(1000)).await;
                }
            }
        }
    }
}

async fn xmpp_session(app: Rc<App>, mut ws: Ws<BoxIo>) {
    let mut x = Xmpp { jid: String::new(), authed: false };
    let mut tmp = vec![0u8; 16384];
    loop {
        if ws.destroyed {
            return;
        }
        match ws.io.read(&mut tmp).await {
            // half-open like Node's http.Server: after the client's FIN the socket stays open (nothing more is sent)
            Ok(0) if !ws.ended => return std::future::pending().await,
            Ok(0) | Err(_) => return,
            Ok(n) => ws.buf.extend_from_slice(&tmp[..n]),
        }
        while let Some(f) = ws.next_frame() {
            if let Some(text) = ws.handle_frame(f).await {
                for out in x.on_text(&app, &text) {
                    match out {
                        Out::Send(s) => ws.send(&s).await,
                        Out::Close => ws.close(None).await,
                    }
                }
            }
            if ws.destroyed {
                return;
            }
        }
    }
}

#[derive(Debug, PartialEq)]
enum Out {
    Send(String),
    Close,
}

/// Enough XMPP for the client to log in, bind and send presence; messages are accepted and dropped.
struct Xmpp {
    jid: String,
    authed: bool,
}

/// `x.match(new RegExp(`${n}=["']([^"']*)["']`))[1] || ''`
fn attr(x: &str, n: &str) -> String {
    let pat = format!("{}=", n);
    let mut from = 0;
    while let Some(p) = x[from..].find(&pat) {
        let q = from + p + pat.len();
        if matches!(x.as_bytes().get(q), Some(b'"') | Some(b'\'')) {
            if let Some(e) = x[q + 1..].find(|c: char| c == '"' || c == '\'') {
                return x[q + 1..q + 1 + e].to_string();
            }
        }
        from += p + 1;
    }
    String::new()
}

/// `(x.match(/>([^<]+)</) || [])[1] || ''`
fn auth_payload(x: &str) -> String {
    let mut from = 0;
    while let Some(p) = x[from..].find('>') {
        let s = from + p + 1;
        if let Some(e) = x[s..].find('<') {
            if e >= 1 {
                return x[s..s + e].to_string();
            }
        } else {
            break;
        }
        from = s;
    }
    String::new()
}

/// `(x.match(/<resource>([^<]*)<\/resource>/) || [])[1] || 'V2:Fortnite:WIN'`
fn resource(x: &str) -> String {
    let mut from = 0;
    while let Some(p) = x[from..].find("<resource>") {
        let s = from + p + "<resource>".len();
        let e = x[s..].find('<').map_or(x.len(), |e| s + e);
        if x[e..].starts_with("</resource>") && e > s {
            return x[s..e].to_string();
        }
        if x[e..].starts_with("</resource>") {
            return "V2:Fortnite:WIN".into();
        }
        from = s;
    }
    "V2:Fortnite:WIN".into()
}

/// String.prototype.replace(string, replacement) for the first match, with the replacement's $ patterns
fn replace_first(s: &str, pat: &str, rep: &str) -> String {
    let Some(pos) = s.find(pat) else { return s.to_string() };
    let mut r = String::new();
    let mut it = rep.chars().peekable();
    while let Some(c) = it.next() {
        if c != '$' {
            r.push(c);
            continue;
        }
        match it.peek().copied() {
            Some('$') => {
                it.next();
                r.push('$');
            }
            Some('&') => {
                it.next();
                r.push_str(pat);
            }
            Some('`') => {
                it.next();
                r.push_str(&s[..pos]);
            }
            Some('\'') => {
                it.next();
                r.push_str(&s[pos + pat.len()..]);
            }
            _ => r.push('$'),
        }
    }
    format!("{}{}{}", &s[..pos], r, &s[pos + pat.len()..])
}

/// `.replace(/ to="[^"]*"( to=)/, '$1')`
fn drop_first_to(s: &str) -> String {
    let mut from = 0;
    while let Some(p) = s[from..].find(" to=\"") {
        let a = from + p;
        let v = a + 5;
        if let Some(q) = s[v..].find('"') {
            let after = v + q + 1;
            if s[after..].starts_with(" to=") {
                return format!("{}{}", &s[..a], &s[after..]);
            }
        }
        from = a + 1;
    }
    s.to_string()
}

fn is_hex32(s: &str) -> bool {
    s.len() == 32 && s.bytes().all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(&c))
}

impl Xmpp {
    fn on_text(&mut self, app: &App, x: &str) -> Vec<Out> {
        let mut out = Vec::new();
        if x.starts_with("<open") {
            out.push(Out::Send(format!(
                "<open xmlns=\"urn:ietf:params:xml:ns:xmpp-framing\" from=\"{}\" id=\"{}\" version=\"1.0\" xml:lang=\"en\"/>",
                DOMAIN,
                js::random_hex(8)
            )));
            out.push(Out::Send(if self.authed {
                "<stream:features xmlns:stream=\"http://etherx.jabber.org/streams\"><bind xmlns=\"urn:ietf:params:xml:ns:xmpp-bind\"/><session xmlns=\"urn:ietf:params:xml:ns:xmpp-session\"><optional/></session></stream:features>".into()
            } else {
                "<stream:features xmlns:stream=\"http://etherx.jabber.org/streams\"><mechanisms xmlns=\"urn:ietf:params:xml:ns:xmpp-sasl\"><mechanism>PLAIN</mechanism></mechanisms><ver xmlns=\"urn:xmpp:features:rosterver\"/></stream:features>".into()
            }));
        } else if x.starts_with("<auth") {
            let plain = String::from_utf8_lossy(&js::base64_decode_lenient(&auth_payload(x))).into_owned();
            let parts: Vec<&str> = plain.split('\0').collect();
            let who = parts.get(1).copied().unwrap_or("").split('@').next().unwrap_or("");
            let who = if who.is_empty() { app.account_id.as_str() } else { who };
            self.jid = format!("{}@{}", if is_hex32(who) { who } else { &app.account_id }, DOMAIN);
            self.authed = true;
            out.push(Out::Send("<success xmlns=\"urn:ietf:params:xml:ns:xmpp-sasl\"/>".into()));
        } else if x.starts_with("<iq") {
            let iq_id = attr(x, "id");
            if x.contains("urn:ietf:params:xml:ns:xmpp-bind") {
                let res = resource(x);
                self.jid = format!("{}/{}", self.jid.split('/').next().unwrap_or(""), res);
                out.push(Out::Send(format!(
                    "<iq type=\"result\" id=\"{}\" to=\"{}\" xmlns=\"jabber:client\"><bind xmlns=\"urn:ietf:params:xml:ns:xmpp-bind\"><jid>{}</jid></bind></iq>",
                    iq_id, self.jid, self.jid
                )));
            } else {
                out.push(Out::Send(format!(
                    "<iq type=\"result\" id=\"{}\" to=\"{}\" from=\"{}\" xmlns=\"jabber:client\"/>",
                    iq_id, self.jid, DOMAIN
                )));
            }
        } else if x.starts_with("<presence") {
            let bare = self.jid.split('/').next().unwrap_or("");
            let rep = format!("<presence from=\"{}\" to=\"{}\"", self.jid, bare);
            out.push(Out::Send(drop_first_to(&replace_first(x, "<presence", &rep))));
        } else if x.starts_with("<close") {
            out.push(Out::Send("<close xmlns=\"urn:ietf:params:xml:ns:xmpp-framing\"/>".into()));
            out.push(Out::Close);
        }
        // <message/> and anything else: accepted and dropped (no other players here)
        out
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn xml_helpers_follow_the_js_regexes() {
        assert_eq!(attr("<iq type='set' id='bind_1'>", "id"), "bind_1");
        assert_eq!(attr("<iq sid=\"x\" id=\"y\">", "id"), "x");
        assert_eq!(attr("<iq id=abc id=\"q\">", "id"), "q");
        assert_eq!(attr("<iq>", "id"), "");
        assert_eq!(auth_payload("<auth mechanism='PLAIN' xmlns='urn:ietf:params:xml:ns:xmpp-sasl'>AGFiYwBwdw==</auth>"), "AGFiYwBwdw==");
        assert_eq!(auth_payload("<a><b>>x<"), ">x");
        assert_eq!(auth_payload("<auth/>"), "");
        assert_eq!(resource("<bind><resource>V2:Fortnite:WIN::ABC</resource></bind>"), "V2:Fortnite:WIN::ABC");
        assert_eq!(resource("<bind><resource></resource></bind>"), "V2:Fortnite:WIN");
        assert_eq!(resource("<bind/>"), "V2:Fortnite:WIN");
        assert_eq!(replace_first("<presence><x/></presence>", "<presence", "<presence a=\"$$\" b=\"$&\""),
            "<presence a=\"$\" b=\"<presence\"><x/></presence>");
        assert_eq!(drop_first_to("<presence from=\"a\" to=\"b\" to=\"c\"/>"), "<presence from=\"a\" to=\"c\"/>");
        assert_eq!(drop_first_to("<presence from=\"a\" to=\"b\"/>"), "<presence from=\"a\" to=\"b\"/>");
    }
}
