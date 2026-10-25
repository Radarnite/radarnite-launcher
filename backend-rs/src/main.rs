//! Fortnite offline backend: a tiny local stand-in for Epic's services, for old Fortnite builds played offline.
//! A behavioural port of backend/server.mjs (same command line, same answers, same files):
//!
//! - Listens ONLY on 127.0.0.1 (never on the network) and makes NO outgoing connections.
//! - One local account; name and id from launcher/config.json. No real Epic account, token or credential is used.
//! - Every cosmetic of the running build owned, with every style: <build>/offline/cosmetics.json.
//! - Locker changes are kept in <build>/offline/state.json.
//!
//! usage: radarnite-backend --build <build root> --config <config.json> [--port 3551] [--no-match]
#![recursion_limit = "512"]

mod app;
mod cosmetics;
mod js;
mod mcp;
mod routes;
mod trace;
mod ws;

use app::App;
use bytes::Bytes;
use http_body_util::{BodyExt, Full};
use hyper::body::Incoming;
use hyper::header::{HeaderMap, CONNECTION, UPGRADE};
use hyper::server::conn::http1;
use hyper::service::service_fn;
use hyper::{Method, Request, Response, Version};
use hyper_util::rt::{TokioIo, TokioTimer};
use routes::Resp;
use std::io;
use std::net::Ipv4Addr;
use std::rc::Rc;
use std::time::Duration;

fn main() {
    trace::start();
    let args: Vec<String> = std::env::args_os().map(|a| a.to_string_lossy().into_owned()).collect();
    let app = match App::start(&args) {
        Ok(a) => a,
        Err((code, msg)) => {
            trace::mark("exit");
            trace::flush();
            eprintln!("{}", msg);
            std::process::exit(code);
        }
    };
    trace::mark("app ready");
    // a panicking route answers 500 (routes::dispatch catches it); nothing goes to stderr
    std::panic::set_hook(Box::new(|_| {}));
    let rt = match tokio::runtime::Builder::new_current_thread().enable_all().build() {
        Ok(rt) => rt,
        Err(e) => {
            eprintln!("{}", e);
            std::process::exit(1);
        }
    };
    trace::mark("runtime built");
    let local = tokio::task::LocalSet::new();
    local.block_on(&rt, serve(Rc::new(app)));
}

/// Node's text for a listen error ("listen EADDRINUSE: address already in use 127.0.0.1:3551")
fn listen_error(e: &io::Error, port: u16) -> String {
    match e.kind() {
        io::ErrorKind::AddrInUse => format!("listen EADDRINUSE: address already in use 127.0.0.1:{}", port),
        io::ErrorKind::PermissionDenied => format!("listen EACCES: permission denied 127.0.0.1:{}", port),
        io::ErrorKind::AddrNotAvailable => format!("listen EADDRNOTAVAIL: address not available 127.0.0.1:{}", port),
        _ => format!("listen {}: 127.0.0.1:{}", e, port),
    }
}

async fn serve(app: Rc<App>) {
    let port = app.port as u16;
    let listener = match tokio::net::TcpListener::bind((Ipv4Addr::LOCALHOST, port)).await {
        Ok(l) => {
            trace::mark("bound");
            l
        }
        Err(e) => {
            let m = listen_error(&e, port);
            app.log(&format!("server error {}", m));
            eprintln!("{}", m);
            std::process::exit(1);
        }
    };
    app.log(&format!(
        "listening on 127.0.0.1:{} as {} ({}), {} cosmetics",
        port,
        app.display_name,
        app.account_id,
        app.cos.count
    ));
    trace::mark("logged");
    println!("ready 127.0.0.1:{}", port);
    trace::mark("ready printed");
    trace::flush();
    loop {
        let stream = match listener.accept().await {
            Ok((s, _)) => s,
            Err(_) => {
                tokio::time::sleep(Duration::from_millis(10)).await;
                continue;
            }
        };
        let _ = stream.set_nodelay(true);
        let app = app.clone();
        tokio::task::spawn_local(async move {
            match ws::sniff(&stream).await {
                ws::Sniff::Closed => return,
                ws::Sniff::Upgrade(u, n) => return ws::raw_upgrade(app, stream, n, u).await,
                ws::Sniff::Http => {}
            }
            let svc = service_fn(move |req| handle(app.clone(), req));
            let mut b = http1::Builder::new();
            // Node's answers: Title-Case headers, its own Date, idle keep-alive connections closed after 5 s
            b.title_case_headers(true)
                .auto_date_header(false)
                .timer(TokioTimer::new())
                .header_read_timeout(Duration::from_secs(5));
            let _ = b.serve_connection(TokioIo::new(stream), svc).with_upgrades().await;
        });
    }
}

fn has_token(h: &HeaderMap, name: hyper::header::HeaderName, token: &str) -> bool {
    h.get_all(name)
        .iter()
        .any(|v| v.to_str().map_or(false, |s| s.split(',').any(|t| t.trim().eq_ignore_ascii_case(token))))
}

fn first_header(h: &HeaderMap, name: &str) -> Option<String> {
    h.get(name).map(|v| js::latin1(v.as_bytes()))
}

async fn handle(app: Rc<App>, req: Request<Incoming>) -> Result<Response<Full<Bytes>>, io::Error> {
    let raw_url = req.uri().to_string();
    if req.method() == Method::CONNECT {
        return Err(io::Error::other("CONNECT"));
    }
    let h = req.headers();
    if h.contains_key(UPGRADE) && has_token(h, CONNECTION, "upgrade") {
        return ws::upgrade(app, req, &raw_url);
    }
    let keep_alive = match req.version() {
        Version::HTTP_10 => has_token(h, CONNECTION, "keep-alive"),
        _ => !has_token(h, CONNECTION, "close"),
    };
    let ua = first_header(h, "user-agent").unwrap_or_default();
    let auth = first_header(h, "authorization");
    let method = req.method().as_str().to_string();
    // like server.mjs: at most 4096 chunks of the body are kept
    let mut body = Vec::new();
    let mut chunks = 0usize;
    let mut incoming = req.into_body();
    while let Some(frame) = incoming.frame().await {
        let frame = frame.map_err(io::Error::other)?;
        if let Ok(d) = frame.into_data() {
            if chunks < 4096 {
                body.extend_from_slice(&d);
            }
            chunks += 1;
        }
    }
    let resp = app.dispatch(method, raw_url, ua, auth, body);
    Ok(response(resp, keep_alive))
}

/// Node's header set and order: Content-Type, Content-Length, Date, Connection, Keep-Alive
fn response(r: Resp, keep_alive: bool) -> Response<Full<Bytes>> {
    let (code, body, ctype) = match r {
        Resp::Json(c, s) => (c, s.into_bytes(), Some("application/json")),
        Resp::Body(c, b, t) => (c, b, Some(t)),
        Resp::Empty => (204, Vec::new(), None),
    };
    let mut b = Response::builder().status(code);
    if let Some(t) = ctype {
        b = b.header("Content-Type", t).header("Content-Length", body.len());
    }
    b = b.header("Date", js::http_date(js::now_ms()));
    b = if keep_alive {
        b.header("Connection", "keep-alive").header("Keep-Alive", "timeout=5")
    } else {
        b.header("Connection", "close")
    };
    b.body(Full::new(Bytes::from(body))).unwrap_or_else(|_| Response::new(Full::new(Bytes::new())))
}
