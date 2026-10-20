//! The JavaScript semantics server.mjs relies on, so the port answers byte for byte like Node:
//! truthiness, String(), Number(), JSON.parse / JSON.stringify (V8 key order, JS number format),
//! Date.prototype.toISOString, decodeURIComponent, the WHATWG URL pathname and URLSearchParams,
//! Node's lenient base64 decoder and path.basename.

use serde_json::{Map, Number, Value};
use std::io;
use std::time::{SystemTime, UNIX_EPOCH};

pub type Obj = Map<String, Value>;

/// A thrown JS error: what server.mjs's per-route catch turns into a 500.
#[derive(Debug, Clone)]
pub struct JsError {
    pub name: &'static str,
    pub message: String,
}
pub type JsResult<T> = Result<T, JsError>;

pub fn type_error(message: impl Into<String>) -> JsError {
    JsError { name: "TypeError", message: message.into() }
}

// ---------- values ----------

pub fn truthy(v: Option<&Value>) -> bool {
    match v {
        None | Some(Value::Null) => false,
        Some(Value::Bool(b)) => *b,
        Some(Value::Number(n)) => n.as_f64().map_or(false, |f| f != 0.0 && !f.is_nan()),
        Some(Value::String(s)) => !s.is_empty(),
        Some(_) => true,
    }
}

/// `a || b`
pub fn or(a: Option<Value>, b: Value) -> Value {
    if truthy(a.as_ref()) {
        a.unwrap()
    } else {
        b
    }
}

/// `a ?? b`
pub fn nullish_or(a: Option<Value>, b: Value) -> Value {
    match a {
        Some(v) if !v.is_null() => v,
        _ => b,
    }
}

pub fn type_name(v: &Value) -> &'static str {
    match v {
        Value::Null => "object",
        Value::Bool(_) => "boolean",
        Value::Number(_) => "number",
        Value::String(_) => "string",
        Value::Array(_) | Value::Object(_) => "object",
    }
}

/// String(v)
pub fn to_string(v: Option<&Value>) -> String {
    match v {
        None => "undefined".into(),
        Some(Value::Null) => "null".into(),
        Some(Value::Bool(b)) => b.to_string(),
        Some(Value::Number(n)) => number_to_string(n),
        Some(Value::String(s)) => s.clone(),
        Some(Value::Array(a)) => a
            .iter()
            .map(|e| if e.is_null() { String::new() } else { to_string(Some(e)) })
            .collect::<Vec<_>>()
            .join(","),
        Some(Value::Object(_)) => "[object Object]".into(),
    }
}

pub fn number_to_string(n: &Number) -> String {
    if let Some(i) = n.as_i64() {
        return i.to_string();
    }
    if let Some(u) = n.as_u64() {
        return u.to_string();
    }
    num_to_string(n.as_f64().unwrap_or(f64::NAN))
}

/// Number.prototype.toString() for a double (shortest round-trip digits, JS exponent rules).
pub fn num_to_string(f: f64) -> String {
    if f.is_nan() {
        return "NaN".into();
    }
    if f.is_infinite() {
        return if f > 0.0 { "Infinity".into() } else { "-Infinity".into() };
    }
    if f == 0.0 {
        return "0".into();
    }
    let a = f.abs();
    if a >= 1e21 || a < 1e-6 {
        let mut s = format!("{:e}", f);
        if let Some(p) = s.find('e') {
            if !s[p + 1..].starts_with('-') {
                s.insert(p + 1, '+');
            }
        }
        s
    } else {
        format!("{}", f)
    }
}

fn is_js_space(c: char) -> bool {
    matches!(c, '\t' | '\n' | '\u{b}' | '\u{c}' | '\r' | ' ' | '\u{a0}' | '\u{1680}' | '\u{2000}'..='\u{200a}'
        | '\u{2028}' | '\u{2029}' | '\u{202f}' | '\u{205f}' | '\u{3000}' | '\u{feff}')
}

/// Number(string)
pub fn number_str(s: &str) -> f64 {
    let t = s.trim_matches(is_js_space);
    if t.is_empty() {
        return 0.0;
    }
    let radix = match t.get(..2) {
        Some("0x") | Some("0X") => 16,
        Some("0o") | Some("0O") => 8,
        Some("0b") | Some("0B") => 2,
        _ => 10,
    };
    if radix != 10 {
        let d = &t[2..];
        if d.is_empty() || !d.chars().all(|c| c.is_digit(radix)) {
            return f64::NAN;
        }
        return d.chars().fold(0.0, |acc, c| acc * radix as f64 + c.to_digit(radix).unwrap() as f64);
    }
    match t {
        "Infinity" | "+Infinity" => return f64::INFINITY,
        "-Infinity" => return f64::NEG_INFINITY,
        _ => {}
    }
    // StrDecimalLiteral: [+-] (digits [. digits] | . digits) [(e|E) [+-] digits]
    let b = t.as_bytes();
    let mut i = 0;
    if i < b.len() && (b[i] == b'+' || b[i] == b'-') {
        i += 1;
    }
    let int_start = i;
    while i < b.len() && b[i].is_ascii_digit() {
        i += 1;
    }
    let mut digits = i - int_start;
    if i < b.len() && b[i] == b'.' {
        i += 1;
        let f = i;
        while i < b.len() && b[i].is_ascii_digit() {
            i += 1;
        }
        digits += i - f;
    }
    if digits == 0 {
        return f64::NAN;
    }
    if i < b.len() && (b[i] == b'e' || b[i] == b'E') {
        i += 1;
        if i < b.len() && (b[i] == b'+' || b[i] == b'-') {
            i += 1;
        }
        let e = i;
        while i < b.len() && b[i].is_ascii_digit() {
            i += 1;
        }
        if i == e {
            return f64::NAN;
        }
    }
    if i != b.len() {
        return f64::NAN;
    }
    t.parse::<f64>().unwrap_or(f64::NAN)
}

/// Number(v)
pub fn to_number(v: Option<&Value>) -> f64 {
    match v {
        None => f64::NAN,
        Some(Value::Null) => 0.0,
        Some(Value::Bool(b)) => {
            if *b {
                1.0
            } else {
                0.0
            }
        }
        Some(Value::Number(n)) => n.as_f64().unwrap_or(f64::NAN),
        Some(Value::String(s)) => number_str(s),
        Some(other) => number_str(&to_string(Some(other))),
    }
}

/// A JSON number for a double, kept integral when it is (so it prints like JS either way).
pub fn num(f: f64) -> Value {
    if f.fract() == 0.0 && f.abs() < 9007199254740992.0 {
        Value::from(f as i64)
    } else {
        Number::from_f64(f).map(Value::Number).unwrap_or(Value::Null)
    }
}

/// `===` between JSON values (objects and arrays are never the same object here).
pub fn strict_eq(a: Option<&Value>, b: Option<&Value>) -> bool {
    match (a, b) {
        (None, None) => true,
        (Some(Value::Null), Some(Value::Null)) => true,
        (Some(Value::Bool(x)), Some(Value::Bool(y))) => x == y,
        (Some(Value::Number(x)), Some(Value::Number(y))) => x.as_f64() == y.as_f64(),
        (Some(Value::String(x)), Some(Value::String(y))) => x == y,
        _ => false,
    }
}

/// `[...new Set(items)]` (undefined items become null in JSON)
pub fn dedupe(items: Vec<Option<Value>>) -> Vec<Value> {
    let mut out: Vec<Option<Value>> = Vec::with_capacity(items.len());
    for it in items {
        let primitive = !matches!(it, Some(Value::Array(_)) | Some(Value::Object(_)));
        if primitive && out.iter().any(|o| strict_eq(o.as_ref(), it.as_ref())) {
            continue;
        }
        out.push(it);
    }
    out.into_iter().map(|v| v.unwrap_or(Value::Null)).collect()
}

/// Canonical array-index key ("0", "17"): V8 orders these first, ascending, in every object.
pub fn is_index_key(k: &str) -> bool {
    if k.is_empty() || k.len() > 10 || !k.bytes().all(|c| c.is_ascii_digit()) {
        return false;
    }
    if k.len() > 1 && k.starts_with('0') {
        return false;
    }
    k.parse::<u64>().map_or(false, |n| n < 4294967295)
}

/// obj[k] = v with V8's property order (existing keys keep their place; index keys go first).
pub fn set(o: &mut Obj, k: &str, v: Value) {
    if let Some(slot) = o.get_mut(k) {
        *slot = v;
        return;
    }
    if !is_index_key(k) {
        o.insert(k.to_string(), v);
        return;
    }
    let n: u64 = k.parse().unwrap();
    let old = std::mem::take(o);
    let mut placed = false;
    for (key, val) in old {
        if !placed && !(is_index_key(&key) && key.parse::<u64>().unwrap() < n) {
            o.insert(k.to_string(), v.clone());
            placed = true;
        }
        o.insert(key, val);
    }
    if !placed {
        o.insert(k.to_string(), v);
    }
}

/// Inserts a possibly-undefined value: undefined properties are omitted by JSON.stringify.
pub fn put_opt(o: &mut Obj, k: &str, v: Option<Value>) {
    match v {
        Some(v) => set(o, k, v),
        None => {
            o.shift_remove(k);
        }
    }
}

/// v[k] for a non-null value (undefined -> None); v must not be null/undefined (see `prop`).
pub fn get(v: &Value, k: &str) -> Option<Value> {
    match v {
        Value::Object(o) => o.get(k).cloned(),
        Value::Array(a) => {
            if k == "length" {
                Some(Value::from(a.len()))
            } else if is_index_key(k) {
                a.get(k.parse::<usize>().ok()?).cloned()
            } else {
                None
            }
        }
        Value::String(s) => {
            if k == "length" {
                Some(Value::from(s.encode_utf16().count()))
            } else if is_index_key(k) {
                let unit = s.encode_utf16().nth(k.parse::<usize>().ok()?)?;
                Some(Value::String(String::from_utf16_lossy(&[unit])))
            } else {
                None
            }
        }
        _ => None,
    }
}

/// v.k with JS's TypeError on null.
pub fn prop(v: &Value, k: &str) -> JsResult<Option<Value>> {
    if v.is_null() {
        return Err(type_error(format!("Cannot read properties of null (reading '{}')", k)));
    }
    Ok(get(v, k))
}

/// `v?.k`
pub fn opt_chain(v: Option<&Value>, k: &str) -> Option<Value> {
    match v {
        None | Some(Value::Null) => None,
        Some(v) => get(v, k),
    }
}

/// target[k] = v (strict mode: TypeError on primitives; properties set on arrays are invisible to JSON).
pub fn put(target: &mut Value, k: &str, v: Option<Value>) -> JsResult<()> {
    match target {
        Value::Object(o) => {
            put_opt(o, k, v);
            Ok(())
        }
        Value::Array(a) => {
            if is_index_key(k) {
                let i: usize = k.parse().unwrap();
                if a.len() <= i {
                    a.resize(i + 1, Value::Null);
                }
                a[i] = v.unwrap_or(Value::Null);
            }
            Ok(())
        }
        Value::Null => Err(type_error(format!("Cannot set properties of null (setting '{}')", k))),
        other => Err(type_error(format!(
            "Cannot create property '{}' on {} '{}'",
            k,
            type_name(other),
            to_string(Some(other))
        ))),
    }
}

/// x.toLowerCase() (expr names x in the TypeError like V8 does)
pub fn lower(v: Option<&Value>, expr: &str) -> JsResult<String> {
    match v {
        Some(Value::String(s)) => Ok(s.to_lowercase()),
        None => Err(type_error("Cannot read properties of undefined (reading 'toLowerCase')")),
        Some(Value::Null) => Err(type_error("Cannot read properties of null (reading 'toLowerCase')")),
        Some(_) => Err(type_error(format!("{}.toLowerCase is not a function", expr))),
    }
}

/// `[...v]`
pub fn spread(v: Option<&Value>, expr: &str) -> JsResult<Vec<Value>> {
    match v {
        Some(Value::Array(a)) => Ok(a.clone()),
        Some(Value::String(s)) => Ok(s.chars().map(|c| Value::String(c.to_string())).collect()),
        None => Err(type_error("undefined is not iterable (cannot read property Symbol(Symbol.iterator))")),
        Some(Value::Null) => Err(type_error("object null is not iterable (cannot read property Symbol(Symbol.iterator))")),
        Some(_) => Err(type_error(format!("{} is not iterable", expr))),
    }
}

/// `{ ...v }` into o
pub fn spread_into(o: &mut Obj, v: &Value) {
    match v {
        Value::Object(m) => {
            for (k, x) in m {
                set(o, k, x.clone());
            }
        }
        Value::Array(a) => {
            for (i, x) in a.iter().enumerate() {
                set(o, &i.to_string(), x.clone());
            }
        }
        Value::String(s) => {
            for (i, c) in s.chars().enumerate() {
                set(o, &i.to_string(), Value::String(c.to_string()));
            }
        }
        _ => {}
    }
}

/// `(x || 1) + 1`
pub fn or_one_plus_one(x: Option<&Value>) -> Value {
    if !truthy(x) {
        return Value::from(2);
    }
    match x.unwrap() {
        Value::Number(n) => match n.as_i64() {
            Some(i) => Value::from(i + 1),
            None => num(n.as_f64().unwrap_or(f64::NAN) + 1.0),
        },
        Value::Bool(_) => Value::from(2),
        other => Value::String(to_string(Some(other)) + "1"),
    }
}

/// s.slice(0, n) counting UTF-16 code units (a split surrogate pair is dropped)
pub fn slice_utf16(s: &str, n: usize) -> String {
    let mut out = String::new();
    let mut used = 0;
    for c in s.chars() {
        let w = c.len_utf16();
        if used + w > n {
            break;
        }
        used += w;
        out.push(c);
    }
    out
}

pub fn obj(v: Value) -> Obj {
    match v {
        Value::Object(o) => o,
        _ => Obj::new(),
    }
}

// ---------- JSON ----------

/// JSON.parse (None on a syntax error), with V8's key order and double-precision numbers.
pub fn parse(s: &str) -> Option<Value> {
    serde_json::from_str::<Value>(s).ok().map(normalize)
}

fn normalize(v: Value) -> Value {
    match v {
        Value::Number(n) => {
            let big = match (n.as_i64(), n.as_u64()) {
                (Some(i), _) => i.unsigned_abs() > (1u64 << 53),
                (None, Some(u)) => u > (1u64 << 53),
                _ => false,
            };
            if big {
                Number::from_f64(n.as_f64().unwrap_or(0.0)).map(Value::Number).unwrap_or(Value::Null)
            } else {
                Value::Number(n)
            }
        }
        Value::Array(a) => Value::Array(a.into_iter().map(normalize).collect()),
        Value::Object(m) => {
            let has_index = m.keys().any(|k| is_index_key(k));
            let mut out = Obj::new();
            if has_index {
                let mut idx: Vec<(u64, String, Value)> = Vec::new();
                let mut rest: Vec<(String, Value)> = Vec::new();
                for (k, v) in m {
                    if is_index_key(&k) {
                        idx.push((k.parse().unwrap(), k, normalize(v)));
                    } else {
                        rest.push((k, normalize(v)));
                    }
                }
                idx.sort_by_key(|e| e.0);
                for (_, k, v) in idx {
                    out.insert(k, v);
                }
                for (k, v) in rest {
                    out.insert(k, v);
                }
            } else {
                for (k, v) in m {
                    out.insert(k, normalize(v));
                }
            }
            Value::Object(out)
        }
        other => other,
    }
}

struct JsFormatter;
impl serde_json::ser::Formatter for JsFormatter {
    fn write_f64<W: ?Sized + io::Write>(&mut self, w: &mut W, v: f64) -> io::Result<()> {
        w.write_all(num_to_string(v).as_bytes())
    }
    fn write_f32<W: ?Sized + io::Write>(&mut self, w: &mut W, v: f32) -> io::Result<()> {
        w.write_all(num_to_string(v as f64).as_bytes())
    }
}

fn stringify_any<T: serde::Serialize + ?Sized>(v: &T) -> String {
    let mut out = Vec::with_capacity(512);
    let mut ser = serde_json::Serializer::with_formatter(&mut out, JsFormatter);
    // serializing a serde_json::Value or Map into a Vec cannot fail
    let _ = v.serialize(&mut ser);
    String::from_utf8(out).unwrap_or_default()
}

/// JSON.stringify
pub fn stringify(v: &Value) -> String {
    stringify_any(v)
}

pub fn stringify_obj(o: &Obj) -> String {
    stringify_any(o)
}

// ---------- time ----------

/// Date.now()
pub fn now_ms() -> i64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_millis() as i64).unwrap_or(0)
}

fn civil_from_days(z: i64) -> (i64, u32, u32) {
    let z = z + 719468;
    let era = z.div_euclid(146097);
    let doe = z.rem_euclid(146097);
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    let y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = (doy - (153 * mp + 2) / 5 + 1) as u32;
    let m = if mp < 10 { mp + 3 } else { mp - 9 } as u32;
    (if m <= 2 { y + 1 } else { y }, m, d)
}

/// new Date(ms).toISOString()
pub fn iso(ms: i64) -> String {
    let days = ms.div_euclid(86_400_000);
    let r = ms.rem_euclid(86_400_000);
    let (y, m, d) = civil_from_days(days);
    format!(
        "{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z",
        y,
        m,
        d,
        r / 3_600_000,
        r / 60_000 % 60,
        r / 1000 % 60,
        r % 1000
    )
}

/// new Date().toISOString()
pub fn now_iso() -> String {
    iso(now_ms())
}

/// The HTTP Date header ("Tue, 20 Oct 2026 17:39:37 GMT")
pub fn http_date(ms: i64) -> String {
    const WD: [&str; 7] = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"];
    const MO: [&str; 12] = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"];
    let days = ms.div_euclid(86_400_000);
    let r = ms.rem_euclid(86_400_000) / 1000;
    let (y, m, d) = civil_from_days(days);
    format!(
        "{}, {:02} {} {:04} {:02}:{:02}:{:02} GMT",
        WD[(days + 4).rem_euclid(7) as usize],
        d,
        MO[(m - 1) as usize],
        y,
        r / 3600,
        r / 60 % 60,
        r % 60
    )
}

// ---------- bytes ----------

pub fn hex(b: &[u8]) -> String {
    const H: &[u8; 16] = b"0123456789abcdef";
    let mut s = String::with_capacity(b.len() * 2);
    for &x in b {
        s.push(H[(x >> 4) as usize] as char);
        s.push(H[(x & 15) as usize] as char);
    }
    s
}

/// crypto.randomBytes(n).toString('hex')
pub fn random_hex(n: usize) -> String {
    let mut b = vec![0u8; n];
    if getrandom::getrandom(&mut b).is_err() {
        // never expected; still unique enough for local ids
        let t = now_ms() as u64 ^ (std::process::id() as u64) << 32;
        for (i, x) in b.iter_mut().enumerate() {
            *x = (t.rotate_left(i as u32 * 7) >> (i % 8)) as u8;
        }
    }
    hex(&b)
}

pub fn md5_hex(b: &[u8]) -> String {
    use md5::Digest;
    hex(&md5::Md5::digest(b))
}

pub fn sha1(b: &[u8]) -> Vec<u8> {
    use sha1::Digest;
    sha1::Sha1::digest(b).to_vec()
}

pub fn sha256_hex(b: &[u8]) -> String {
    use sha2::Digest;
    hex(&sha2::Sha256::digest(b))
}

const B64: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

pub fn base64(b: &[u8]) -> String {
    let mut s = String::with_capacity((b.len() + 2) / 3 * 4);
    for c in b.chunks(3) {
        let n = (c[0] as u32) << 16 | (*c.get(1).unwrap_or(&0) as u32) << 8 | *c.get(2).unwrap_or(&0) as u32;
        s.push(B64[(n >> 18) as usize & 63] as char);
        s.push(B64[(n >> 12) as usize & 63] as char);
        s.push(if c.len() > 1 { B64[(n >> 6) as usize & 63] as char } else { '=' });
        s.push(if c.len() > 2 { B64[n as usize & 63] as char } else { '=' });
    }
    s
}

/// Buffer.from(s, 'base64'): both alphabets, other characters skipped, stops at the first '='.
pub fn base64_decode_lenient(s: &str) -> Vec<u8> {
    let mut out = Vec::with_capacity(s.len() * 3 / 4);
    let mut acc: u32 = 0;
    let mut bits = 0;
    for c in s.bytes() {
        let v = match c {
            b'A'..=b'Z' => c - b'A',
            b'a'..=b'z' => c - b'a' + 26,
            b'0'..=b'9' => c - b'0' + 52,
            b'+' | b'-' => 62,
            b'/' | b'_' => 63,
            b'=' => break,
            _ => continue,
        };
        acc = acc << 6 | v as u32;
        bits += 6;
        if bits >= 8 {
            bits -= 8;
            out.push((acc >> bits) as u8);
            acc &= (1 << bits) - 1;
        }
    }
    out
}

/// Header bytes as Node exposes them (latin1)
pub fn latin1(b: &[u8]) -> String {
    b.iter().map(|&c| c as char).collect()
}

// ---------- URLs ----------

fn hexval(c: u8) -> Option<u8> {
    match c {
        b'0'..=b'9' => Some(c - b'0'),
        b'a'..=b'f' => Some(c - b'a' + 10),
        b'A'..=b'F' => Some(c - b'A' + 10),
        _ => None,
    }
}

/// decodeURIComponent
pub fn decode_uri_component(s: &str) -> JsResult<String> {
    let malformed = || JsError { name: "URIError", message: "URI malformed".into() };
    let b = s.as_bytes();
    let mut out: Vec<u8> = Vec::with_capacity(b.len());
    let mut i = 0;
    let pct = |i: usize| -> Option<u8> {
        if i + 2 < b.len() && b[i] == b'%' {
            Some(hexval(b[i + 1])? << 4 | hexval(b[i + 2])?)
        } else {
            None
        }
    };
    while i < b.len() {
        if b[i] != b'%' {
            out.push(b[i]);
            i += 1;
            continue;
        }
        let first = pct(i).ok_or_else(malformed)?;
        i += 3;
        if first < 0x80 {
            out.push(first);
            continue;
        }
        let n = first.leading_ones() as usize;
        if !(2..=4).contains(&n) {
            return Err(malformed());
        }
        let mut seq = vec![first];
        for _ in 1..n {
            let c = pct(i).ok_or_else(malformed)?;
            if c & 0xC0 != 0x80 {
                return Err(malformed());
            }
            seq.push(c);
            i += 3;
        }
        if std::str::from_utf8(&seq).is_err() {
            return Err(malformed());
        }
        out.extend_from_slice(&seq);
    }
    String::from_utf8(out).map_err(|_| malformed())
}

fn in_path_set(c: char) -> bool {
    (c as u32) < 0x20
        || (c as u32) > 0x7e
        || matches!(c, ' ' | '"' | '#' | '<' | '>' | '?' | '^' | '`' | '{' | '}')
}

fn is_single_dot(s: &str) -> bool {
    s == "." || s.eq_ignore_ascii_case("%2e")
}

fn is_double_dot(s: &str) -> bool {
    let l = s.to_ascii_lowercase();
    matches!(l.as_str(), ".." | ".%2e" | "%2e." | "%2e%2e")
}

/// A request target's authority that `new URL()` rejects (empty or forbidden host, broken IPv6, bad port): server.mjs
/// would throw ERR_INVALID_URL there. Not the whole WHATWG host parser: percent-encoded, IDNA and IPv4 forms pass.
fn valid_authority(a: &str) -> bool {
    let hostport = a.rfind('@').map_or(a, |i| &a[i + 1..]);
    let (host, port) = if hostport.starts_with('[') {
        match hostport.find(']') {
            Some(i) => (&hostport[..=i], &hostport[i + 1..]),
            None => return false,
        }
    } else {
        match hostport.rfind(':') {
            Some(i) => (&hostport[..i], &hostport[i..]),
            None => (hostport, ""),
        }
    };
    if let Some(p) = port.strip_prefix(':') {
        if !p.bytes().all(|c| c.is_ascii_digit()) || (!p.is_empty() && p.parse::<u32>().map_or(true, |n| n > 65535)) {
            return false;
        }
    } else if !port.is_empty() {
        return false;
    }
    if let Some(v6) = host.strip_prefix('[') {
        return v6.strip_suffix(']').map_or(false, |v| v.parse::<std::net::Ipv6Addr>().is_ok());
    }
    !host.is_empty()
        && !host.chars().any(|c| {
            (c as u32) < 0x21 || c == '\u{7f}' || matches!(c, '#' | '/' | ':' | '<' | '>' | '?' | '@' | '[' | '\\' | ']' | '^' | '|')
        })
}

/// `new URL(req.url, 'http://127.0.0.1')` -> (pathname, search without '?'); None where it throws ERR_INVALID_URL.
pub fn url_parse(raw: &str) -> Option<(String, Option<String>)> {
    let s: String = raw
        .trim_matches(|c: char| c <= ' ')
        .chars()
        .filter(|&c| c != '\t' && c != '\n' && c != '\r')
        .collect();
    let mut rest: &str = &s;
    let lower = s.to_ascii_lowercase();
    let scheme_len = ["http:", "https:", "ws:", "wss:", "ftp:"]
        .iter()
        .find(|p| lower.starts_with(*p))
        .map(|p| p.len());
    let slash = |c: char| c == '/' || c == '\\';
    // the authority after the slashes, and where the path starts
    let authority = |r: &str| -> (usize, usize) {
        let r2 = r.trim_start_matches(slash);
        let skipped = r.len() - r2.len();
        (skipped, skipped + r2.find(|c: char| slash(c) || c == '?' || c == '#').unwrap_or(r2.len()))
    };
    if scheme_len.is_some() || (rest.len() >= 2 && rest.starts_with(slash) && rest[1..].starts_with(slash)) {
        rest = &rest[scheme_len.unwrap_or(0)..];
        let (a, k) = authority(rest);
        if !valid_authority(&rest[a..k]) {
            return None;
        }
        rest = &rest[k..];
    }
    let rest = rest.split('#').next().unwrap_or("");
    let (path, query) = match rest.find('?') {
        Some(i) => (&rest[..i], Some(rest[i + 1..].to_string())),
        None => (rest, None),
    };
    let p = path.strip_prefix(slash).unwrap_or(path);
    let mut segs: Vec<String> = Vec::new();
    let mut buf = String::new();
    let chars: Vec<char> = p.chars().collect();
    for i in 0..=chars.len() {
        let c = chars.get(i).copied();
        if c.map_or(true, slash) {
            let at_slash = c.is_some();
            if is_double_dot(&buf) {
                segs.pop();
                if !at_slash {
                    segs.push(String::new());
                }
            } else if is_single_dot(&buf) {
                if !at_slash {
                    segs.push(String::new());
                }
            } else {
                segs.push(std::mem::take(&mut buf));
            }
            buf.clear();
        } else {
            let c = c.unwrap();
            if in_path_set(c) {
                let mut tmp = [0u8; 4];
                for b in c.encode_utf8(&mut tmp).bytes() {
                    buf.push_str(&format!("%{:02X}", b));
                }
            } else {
                buf.push(c);
            }
        }
    }
    Some((format!("/{}", segs.join("/")), query))
}

fn form_decode(s: &str) -> String {
    let b = s.as_bytes();
    let mut out = Vec::with_capacity(b.len());
    let mut i = 0;
    while i < b.len() {
        match b[i] {
            b'+' => out.push(b' '),
            b'%' if i + 2 < b.len() && hexval(b[i + 1]).is_some() && hexval(b[i + 2]).is_some() => {
                out.push(hexval(b[i + 1]).unwrap() << 4 | hexval(b[i + 2]).unwrap());
                i += 2;
            }
            c => out.push(c),
        }
        i += 1;
    }
    String::from_utf8_lossy(&out).into_owned()
}

/// URLSearchParams entries of a query string (without the '?')
pub fn search_params(q: &str) -> Vec<(String, String)> {
    q.split('&')
        .filter(|s| !s.is_empty())
        .map(|seq| match seq.find('=') {
            Some(i) => (form_decode(&seq[..i]), form_decode(&seq[i + 1..])),
            None => (form_decode(seq), String::new()),
        })
        .collect()
}

/// path.basename (path.win32 on Windows, path.posix elsewhere, like Node on each)
pub fn basename(p: &str) -> String {
    let sep = |c: char| if cfg!(windows) { c == '/' || c == '\\' } else { c == '/' };
    let chars: Vec<char> = p.chars().collect();
    let mut start = 0;
    if cfg!(windows) && chars.len() >= 2 && chars[0].is_ascii_alphabetic() && chars[1] == ':' {
        start = 2;
    }
    let mut end: Option<usize> = None;
    let mut i = chars.len();
    while i > start {
        i -= 1;
        if sep(chars[i]) {
            if end.is_some() {
                start = i + 1;
                break;
            }
        } else if end.is_none() {
            end = Some(i + 1);
        }
    }
    match end {
        None => String::new(),
        Some(e) => chars[start..e].iter().collect(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn iso_matches_js() {
        assert_eq!(iso(0), "1970-01-01T00:00:00.000Z");
        assert_eq!(iso(1792517966123), "2026-10-20T17:39:26.123Z");
        assert_eq!(iso(951782400000), "2000-02-29T00:00:00.000Z");
        assert_eq!(iso(253402300799999), "9999-12-31T23:59:59.999Z");
        assert_eq!(http_date(1792517977000), "Tue, 20 Oct 2026 17:39:37 GMT");
    }

    #[test]
    fn numbers_print_like_js() {
        assert_eq!(num_to_string(1.0), "1");
        assert_eq!(num_to_string(0.1), "0.1");
        assert_eq!(num_to_string(1e21), "1e+21");
        assert_eq!(num_to_string(1.5e-7), "1.5e-7");
        assert_eq!(num_to_string(0.000001), "0.000001");
        assert_eq!(num_to_string(-0.0), "0");
        assert_eq!(num_to_string(123456789012345680000.0), "123456789012345680000");
        assert_eq!(stringify(&parse(r#"{"a":1.0,"b":1e2,"c":9007199254740993,"d":-0,"e":0.5}"#).unwrap()),
            r#"{"a":1,"b":100,"c":9007199254740992,"d":0,"e":0.5}"#);
        assert_eq!(number_str(" 3551 "), 3551.0);
        assert_eq!(number_str("0x10"), 16.0);
        assert!(number_str("inf").is_nan());
        assert!(number_str("12abc").is_nan());
        assert_eq!(number_str(""), 0.0);
        assert_eq!(number_str("1e3"), 1000.0);
        assert_eq!(to_string(Some(&json!(["a", null, 1]))), "a,,1");
    }

    #[test]
    fn index_keys_go_first() {
        let v = parse(r#"{"b":1,"2":2,"a":3,"1":4,"01":5}"#).unwrap();
        assert_eq!(stringify(&v), r#"{"1":4,"2":2,"b":1,"a":3,"01":5}"#);
        let mut o = obj(parse(r#"{"x":1,"5":2}"#).unwrap());
        set(&mut o, "3", json!(0));
        set(&mut o, "y", json!(9));
        set(&mut o, "x", json!(7));
        assert_eq!(stringify_obj(&o), r#"{"3":0,"5":2,"x":7,"y":9}"#);
    }

    #[test]
    fn strings_escape_like_json_stringify() {
        assert_eq!(stringify(&json!("a\"b\\c\n\u{1}\u{7f}é\u{2028}")), "\"a\\\"b\\\\c\\n\\u0001\u{7f}é\u{2028}\"");
    }

    #[test]
    fn url_pathname_like_whatwg() {
        assert_eq!(url_parse("/a/b?x=1").unwrap().0, "/a/b");
        assert_eq!(url_parse("/a/b?x=1").unwrap().1.as_deref(), Some("x=1"));
        assert_eq!(url_parse("/a/../offline/health").unwrap().0, "/offline/health");
        assert_eq!(url_parse("/a/%2E%2e/b").unwrap().0, "/b");
        assert_eq!(url_parse("/a/./b/.").unwrap().0, "/a/b/");
        assert_eq!(url_parse("/..").unwrap().0, "/");
        assert_eq!(url_parse("/").unwrap().0, "/");
        assert_eq!(url_parse("/a//b").unwrap().0, "/a//b");
        assert_eq!(url_parse("/a\\b").unwrap().0, "/a/b");
        assert_eq!(url_parse("/a b{}\"é").unwrap().0, "/a%20b%7B%7D%22%C3%A9");
        assert_eq!(url_parse("*").unwrap().0, "/*");
        assert_eq!(url_parse("http://h:1/x/y?q").unwrap().0, "/x/y");
        assert_eq!(url_parse("//h/x").unwrap().0, "/x");
        assert_eq!(url_parse("/x#frag?no").unwrap().0, "/x");
        assert_eq!(url_parse("//user@h:80/p").unwrap().0, "/p");
        assert_eq!(url_parse("//[::1]:3551/p").unwrap().0, "/p");
        for bad in ["//[x", "//[x/offline/health", "//", "//h:99999/", "//h:a/", "//a<b/", "//[::1/x", "http://[x/a"] {
            assert!(url_parse(bad).is_none(), "{}", bad);
        }
    }

    #[test]
    fn search_params_like_urlsearchparams() {
        let p = search_params("a=1&b=x+y%20z&&c&d=%zz&a=2");
        assert_eq!(p, vec![("a".into(), "1".into()), ("b".into(), "x y z".into()), ("c".into(), "".into()),
            ("d".into(), "%zz".into()), ("a".into(), "2".into())]);
    }

    #[test]
    fn decode_uri_component_like_js() {
        assert_eq!(decode_uri_component("a%20b%2F%C3%A9+").unwrap(), "a b/é+");
        assert!(decode_uri_component("%").is_err());
        assert!(decode_uri_component("%zz").is_err());
        assert!(decode_uri_component("%C3").is_err());
        assert!(decode_uri_component("%ED%A0%80").is_err());
        assert_eq!(decode_uri_component("%E2%82%AC").unwrap(), "€");
    }

    #[test]
    fn base64_like_node() {
        assert_eq!(base64(b"hello"), "aGVsbG8=");
        assert_eq!(base64(&sha1(b"dGhlIHNhbXBsZSBub25jZQ==258EAFA5-E914-47DA-95CA-C5AB0DC85B11")), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
        assert_eq!(base64_decode_lenient("AHVzZXIAcGFzcw=="), b"\0user\0pass");
        assert_eq!(base64_decode_lenient("aGVs bG8"), b"hello");
        assert_eq!(base64_decode_lenient("aGVsbG8=aGVsbG8="), b"hello");
        assert_eq!(base64_decode_lenient("_-8"), vec![0xff, 0xef]);
    }

    #[test]
    fn basename_like_node() {
        assert_eq!(basename("/a/b/14.30"), "14.30");
        assert_eq!(basename("/a/b/14.30/"), "14.30");
        assert_eq!(basename(""), "");
        assert_eq!(basename("/"), "");
        assert_eq!(basename(".."), "..");
        if cfg!(windows) {
            assert_eq!(basename("S:\\FortniteBuilds\\17.50"), "17.50");
            assert_eq!(basename("C:foo"), "foo");
        } else {
            assert_eq!(basename("a\\b"), "a\\b");
        }
    }

    #[test]
    fn js_helpers() {
        assert!(!truthy(Some(&json!(0))));
        assert!(!truthy(Some(&json!(""))));
        assert!(truthy(Some(&json!([]))));
        assert!(truthy(Some(&json!({}))));
        assert_eq!(slice_utf16("abc", 2), "ab");
        assert_eq!(slice_utf16("a😀b", 2), "a");
        assert_eq!(dedupe(vec![Some(json!("a")), Some(json!("a")), None, Some(json!(1)), Some(json!(1.0)), None]),
            vec![json!("a"), json!(null), json!(1)]);
        assert_eq!(or_one_plus_one(Some(&json!(4))), json!(5));
        assert_eq!(or_one_plus_one(None), json!(2));
        assert_eq!(or_one_plus_one(Some(&json!("5"))), json!("51"));
        let mut s = json!("abc");
        assert_eq!(put(&mut s, "x", Some(json!(1))).unwrap_err().message, "Cannot create property 'x' on string 'abc'");
        assert_eq!(lower(Some(&json!(5)), "item").unwrap_err().message, "item.toLowerCase is not a function");
    }
}
