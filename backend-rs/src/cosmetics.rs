//! cosmetics.json + custom-cosmetics.json at startup (server.mjs lines 31-39 and 88-89), without building the whole
//! document: only what server.mjs reads afterwards is kept (each item's templateId and variants, `source`, the custom
//! keychain). Everything else is still parsed (an invalid file falls back to the default exactly like readJson) but
//! skipped without allocating: a build's custom-cosmetics.json can be ~90 MB of pak file names.

use crate::js::{self, type_error, JsError, JsResult};
use serde_json::Value;
use std::collections::{HashMap, HashSet};
use std::path::Path;

/// A value's bytes as captured (whitespace between tokens dropped), parsed only when needed
struct Raw(Vec<u8>);

impl Raw {
    /// JSON.parse of the value (decoded with replacement characters, numbers and key order like V8)
    fn value(&self) -> Res<Value> {
        let mut v: Value = serde_json::from_str(&String::from_utf8_lossy(&self.0)).map_err(|_| ())?;
        js::normalize_value(&mut v);
        Ok(v)
    }

    /// A string value without escapes: its text without going through serde
    fn plain_str(&self) -> Option<&str> {
        let b = &self.0;
        if b.len() >= 2 && b[0] == b'"' && b[b.len() - 1] == b'"' && !b.contains(&b'\\') {
            std::str::from_utf8(&b[1..b.len() - 1]).ok()
        } else {
            None
        }
    }

    /// JSON.stringify(JSON.parse(raw)) would give these exact bytes back: no escapes, plain integers up to 15 digits,
    /// no duplicate or array-index keys, valid UTF-8. Anything else takes the parse + stringify path.
    fn is_canonical(&self) -> bool {
        let b = &self.0;
        if std::str::from_utf8(b).is_err() || b.contains(&b'\\') {
            return false;
        }
        let mut keys: Vec<Vec<&[u8]>> = Vec::new();
        let mut i = 0;
        while i < b.len() {
            match b[i] {
                b'{' => {
                    keys.push(Vec::new());
                    i += 1;
                }
                b'}' => {
                    keys.pop();
                    i += 1;
                }
                b'"' => {
                    let end = match b[i + 1..].iter().position(|&c| c == b'"') {
                        Some(e) => i + 1 + e,
                        None => return false,
                    };
                    let s = &b[i + 1..end];
                    if b.get(end + 1) == Some(&b':') {
                        let Some(k) = keys.last_mut() else { return false };
                        if k.contains(&s) || std::str::from_utf8(s).map_or(true, js::is_index_key) {
                            return false;
                        }
                        k.push(s);
                    }
                    i = end + 1;
                }
                b'-' | b'0'..=b'9' => {
                    let start = i;
                    while i < b.len() && matches!(b[i], b'-' | b'+' | b'.' | b'e' | b'E' | b'0'..=b'9') {
                        i += 1;
                    }
                    let n = &b[start..i];
                    let digits = n.strip_prefix(b"-").unwrap_or(n);
                    let plain = !digits.is_empty() && digits.len() <= 15 && digits.iter().all(u8::is_ascii_digit);
                    if !plain || (digits.len() > 1 && digits[0] == b'0') || n == b"-0" {
                        return false;
                    }
                }
                _ => i += 1,
            }
        }
        true
    }

    /// `x || []` as the JSON.stringify text
    fn or_empty_array_json(&self) -> Res<Box<str>> {
        if matches!(self.0.as_slice(), b"0" | b"false" | b"null" | b"\"\"") {
            return Ok("[]".into());
        }
        if self.is_canonical() {
            // valid UTF-8 was checked
            return Ok(String::from_utf8_lossy(&self.0).into());
        }
        let v = js::or(Some(self.value()?), Value::Array(Vec::new()));
        Ok(js::stringify(&v).into_boxed_str())
    }
}

/// One element of an `items` list as server.mjs looks at it: an object's templateId, and its variants already as the
/// JSON.stringify text of `it.variants || []` (None: no variants key)
enum Item {
    Object { template_id: Option<Value>, variants: Option<Box<str>> },
    Null,
    /// a string, number, boolean or array: `.templateId` is undefined
    Other,
}

/// An `items` field
enum Items {
    Array(Vec<Item>),
    Null,
    /// string, number or boolean
    Primitive(Value),
    Object,
}

/// The top level of the file
enum Doc {
    Object { items: Option<Items>, source: Option<Value>, keychain: Option<Value> },
    Array,
    Null,
    Primitive(Value),
}

/// Streaming reader of a JSON file: validates it with JSON.parse's grammar while keeping only the values asked for.
/// The file is read in 256 KB blocks; skipped values cost no memory. Invalid UTF-8 inside strings is accepted (Node
/// decodes the file with replacement characters before JSON.parse); captured values are decoded the same way.
struct Scanner<R> {
    r: R,
    buf: Vec<u8>,
    pos: usize,
    len: usize,
    /// bytes of the value being captured
    cap: Option<Vec<u8>>,
}

type Res<T> = Result<T, ()>;

impl<R: std::io::Read> Scanner<R> {
    fn new(r: R) -> Self {
        Scanner { r, buf: vec![0; 256 * 1024], pos: 0, len: 0, cap: None }
    }

    fn fill(&mut self) -> bool {
        if self.pos < self.len {
            return true;
        }
        loop {
            match self.r.read(&mut self.buf) {
                Ok(0) => return false,
                Ok(n) => {
                    self.pos = 0;
                    self.len = n;
                    return true;
                }
                Err(e) if e.kind() == std::io::ErrorKind::Interrupted => continue,
                Err(_) => return false,
            }
        }
    }

    fn peek(&mut self) -> Res<u8> {
        if self.fill() {
            Ok(self.buf[self.pos])
        } else {
            Err(())
        }
    }

    fn next(&mut self) -> Res<u8> {
        let b = self.peek()?;
        self.pos += 1;
        if let Some(c) = self.cap.as_mut() {
            c.push(b);
        }
        Ok(b)
    }

    fn ws(&mut self) {
        while self.fill() {
            let s = &self.buf[self.pos..self.len];
            match s.iter().position(|&b| !matches!(b, b' ' | b'\t' | b'\n' | b'\r')) {
                Some(i) => {
                    self.pos += i;
                    return;
                }
                None => self.pos = self.len,
            }
        }
    }

    fn eof(&mut self) -> bool {
        self.ws();
        !self.fill()
    }

    fn expect(&mut self, b: u8) -> Res<()> {
        if self.next()? == b {
            Ok(())
        } else {
            Err(())
        }
    }

    fn literal(&mut self, word: &[u8]) -> Res<()> {
        word.iter().try_for_each(|&b| self.expect(b))
    }

    fn string(&mut self) -> Res<()> {
        self.expect(b'"')?;
        loop {
            if !self.fill() {
                return Err(());
            }
            let s = &self.buf[self.pos..self.len];
            match s.iter().position(|&b| b == b'"' || b == b'\\' || b < 0x20) {
                None => {
                    if let Some(c) = self.cap.as_mut() {
                        c.extend_from_slice(s);
                    }
                    self.pos = self.len;
                }
                Some(i) => {
                    let b = s[i];
                    if let Some(c) = self.cap.as_mut() {
                        c.extend_from_slice(&s[..=i]);
                    }
                    self.pos += i + 1;
                    match b {
                        b'"' => return Ok(()),
                        b'\\' => match self.next()? {
                            b'"' | b'\\' | b'/' | b'b' | b'f' | b'n' | b'r' | b't' => {}
                            b'u' => {
                                for _ in 0..4 {
                                    if !self.next()?.is_ascii_hexdigit() {
                                        return Err(());
                                    }
                                }
                            }
                            _ => return Err(()),
                        },
                        _ => return Err(()),
                    }
                }
            }
        }
    }

    fn digits(&mut self) -> Res<usize> {
        let mut n = 0;
        while self.fill() && self.buf[self.pos].is_ascii_digit() {
            self.next()?;
            n += 1;
        }
        Ok(n)
    }

    /// -? (0 | [1-9][0-9]*) (\.[0-9]+)? ([eE][+-]?[0-9]+)?
    fn number(&mut self) -> Res<()> {
        if self.peek()? == b'-' {
            self.next()?;
        }
        if self.peek()? == b'0' {
            self.next()?;
        } else if self.digits()? == 0 {
            return Err(());
        }
        if self.fill() && self.buf[self.pos] == b'.' {
            self.next()?;
            if self.digits()? == 0 {
                return Err(());
            }
        }
        if self.fill() && matches!(self.buf[self.pos], b'e' | b'E') {
            self.next()?;
            if self.fill() && matches!(self.buf[self.pos], b'+' | b'-') {
                self.next()?;
            }
            if self.digits()? == 0 {
                return Err(());
            }
        }
        Ok(())
    }

    /// `"key":` with the key decoded (None: a key JSON.parse accepts that cannot be one of ours)
    fn key(&mut self) -> Res<Option<String>> {
        self.ws();
        let outer = self.cap.replace(Vec::new());
        let r = self.string();
        let raw = std::mem::replace(&mut self.cap, outer).unwrap_or_default();
        r?;
        if let Some(c) = self.cap.as_mut() {
            c.extend_from_slice(&raw);
        }
        self.ws();
        self.expect(b':')?;
        Ok(serde_json::from_str::<String>(&String::from_utf8_lossy(&raw)).ok())
    }

    /// Any value, nesting included (iterative: no depth limit, like V8)
    fn skip(&mut self) -> Res<()> {
        let mut stack: Vec<u8> = Vec::new();
        loop {
            self.ws();
            match self.peek()? {
                b'{' => {
                    self.next()?;
                    self.ws();
                    if self.peek()? == b'}' {
                        self.next()?;
                    } else {
                        stack.push(b'}');
                        self.key()?;
                        continue;
                    }
                }
                b'[' => {
                    self.next()?;
                    self.ws();
                    if self.peek()? == b']' {
                        self.next()?;
                    } else {
                        stack.push(b']');
                        continue;
                    }
                }
                b'"' => self.string()?,
                b't' => self.literal(b"true")?,
                b'f' => self.literal(b"false")?,
                b'n' => self.literal(b"null")?,
                b'-' | b'0'..=b'9' => self.number()?,
                _ => return Err(()),
            }
            // a value is complete: close what it completes, or go on with the next element
            loop {
                let Some(&close) = stack.last() else { return Ok(()) };
                self.ws();
                match self.next()? {
                    b',' => {
                        if close == b'}' {
                            self.key()?;
                        }
                        break;
                    }
                    b if b == close => {
                        stack.pop();
                    }
                    _ => return Err(()),
                }
            }
        }
    }

    /// A value's bytes (validated like JSON.parse)
    fn capture(&mut self) -> Res<Raw> {
        self.ws();
        self.cap = Some(Vec::new());
        let r = self.skip();
        let raw = self.cap.take().unwrap_or_default();
        r?;
        Ok(Raw(raw))
    }

    /// `{ ... }` calling f for each key (f must consume the value); `{` already peeked
    fn object(&mut self, mut f: impl FnMut(&mut Self, Option<String>) -> Res<()>) -> Res<()> {
        self.expect(b'{')?;
        self.ws();
        if self.peek()? == b'}' {
            return self.next().map(|_| ());
        }
        loop {
            let k = self.key()?;
            self.ws();
            f(self, k)?;
            self.ws();
            match self.next()? {
                b',' => continue,
                b'}' => return Ok(()),
                _ => return Err(()),
            }
        }
    }

    fn item(&mut self) -> Res<Item> {
        self.ws();
        match self.peek()? {
            b'{' => {
                // duplicate keys: the last one wins, like JSON.parse
                let (mut template_id, mut variants) = (None, None);
                self.object(|s, k| match k.as_deref() {
                    Some("templateId") => {
                        let raw = s.capture()?;
                        template_id = Some(match raw.plain_str() {
                            Some(t) => Value::String(t.to_string()),
                            None => raw.value()?,
                        });
                        Ok(())
                    }
                    Some("variants") => s.capture()?.or_empty_array_json().map(|v| variants = Some(v)),
                    _ => s.skip(),
                })?;
                Ok(Item::Object { template_id, variants })
            }
            b'n' => self.literal(b"null").map(|_| Item::Null),
            _ => self.skip().map(|_| Item::Other),
        }
    }

    fn items(&mut self) -> Res<Items> {
        self.ws();
        match self.peek()? {
            b'[' => {
                self.next()?;
                let mut v = Vec::new();
                self.ws();
                if self.peek()? == b']' {
                    self.next()?;
                    return Ok(Items::Array(v));
                }
                loop {
                    v.push(self.item()?);
                    self.ws();
                    match self.next()? {
                        b',' => continue,
                        b']' => return Ok(Items::Array(v)),
                        _ => return Err(()),
                    }
                }
            }
            b'{' => self.skip().map(|_| Items::Object),
            b'n' => self.literal(b"null").map(|_| Items::Null),
            _ => self.capture()?.value().map(Items::Primitive),
        }
    }

    fn doc(&mut self) -> Res<Doc> {
        self.ws();
        let doc = match self.peek()? {
            b'{' => {
                let (mut items, mut source, mut keychain) = (None, None, None);
                self.object(|s, k| match k.as_deref() {
                    Some("items") => s.items().map(|v| items = Some(v)),
                    Some("source") => s.capture()?.value().map(|v| source = Some(v)),
                    Some("keychain") => s.capture()?.value().map(|v| keychain = Some(v)),
                    _ => s.skip(),
                })?;
                Doc::Object { items, source, keychain }
            }
            b'[' => self.skip().map(|_| Doc::Array)?,
            b'n' => self.literal(b"null").map(|_| Doc::Null)?,
            _ => Doc::Primitive(self.capture()?.value()?),
        };
        if self.eof() {
            Ok(doc)
        } else {
            Err(())
        }
    }
}

/// readJson(f, d) for these two files: None (= the default) when missing or not valid JSON; one leading BOM ignored
fn read_doc(f: &Path) -> Option<Doc> {
    let file = std::fs::File::open(f).ok()?;
    let mut sc = Scanner::new(file);
    if sc.fill() && sc.buf[..sc.len].starts_with(b"\xEF\xBB\xBF") {
        sc.pos = 3;
    }
    let doc = sc.doc().ok();
    crate::trace::mark("  scanned");
    doc
}

/// What the backend keeps of the two files
pub struct Cosmetics {
    /// `(cosmetics.items || []).length` after the custom items were added
    pub count: usize,
    /// `cosmetics.source || 'none'`
    pub source: Value,
    /// `customCosmetics.keychain` (None = undefined)
    pub keychain: Option<Value>,
    /// lower-case templateId -> JSON.stringify(it.variants || []) of its last occurrence
    pub variants: HashMap<String, Box<str>>,
    /// the athena profile's items object, in V8's key order: one entry per distinct id
    pub athena_keys: Vec<AthenaKey>,
}

pub enum AthenaKey {
    /// (lower-case id, templateId of the id's last occurrence)
    Cosmetic(String, String),
    Default(&'static str),
    Sandbox,
}

fn not_iterable(what: &str) -> JsError {
    type_error(format!("{} is not iterable", what))
}

/// The template id of an item that must have a string one (else server.mjs throws at startup)
fn template_of(it: &Item, expr: &str) -> JsResult<String> {
    match it {
        Item::Object { template_id: Some(Value::String(s)), .. } => Ok(s.clone()),
        Item::Null => Err(type_error("Cannot read properties of null (reading 'templateId')")),
        Item::Object { template_id: Some(v), .. } if !v.is_null() => Err(type_error(format!("{}.toLowerCase is not a function", expr))),
        Item::Object { template_id: Some(_), .. } => Err(type_error("Cannot read properties of null (reading 'toLowerCase')")),
        _ => Err(type_error("Cannot read properties of undefined (reading 'toLowerCase')")),
    }
}

/// Loads both files with server.mjs's startup semantics (Err = the TypeError that makes it exit)
pub fn load(off: &Path, athena_defaults: &[(&str, &'static str)]) -> JsResult<Cosmetics> {
    crate::trace::mark("cosmetics.json:");
    let (items, source) = match read_doc(&off.join("cosmetics.json")) {
        None => (Some(Items::Array(Vec::new())), None),
        Some(Doc::Object { items, source, .. }) => (items, source),
        // `cosmetics.items ??= []` on an array sets an invisible property
        Some(Doc::Array) => (None, None),
        Some(Doc::Null) => return Err(type_error("Cannot read properties of null (reading 'items')")),
        Some(Doc::Primitive(v)) => {
            return Err(type_error(format!("Cannot create property 'items' on {} '{}'", js::type_name(&v), js::to_string(Some(&v)))))
        }
    };
    // cosmetics.items ??= []
    let mut items = match items {
        None | Some(Items::Null) => Items::Array(Vec::new()),
        Some(x) => x,
    };
    crate::trace::mark("custom-cosmetics.json:");
    let (citems, keychain) = match read_doc(&off.join("custom-cosmetics.json")) {
        None => (None, Some(Value::Array(Vec::new()))),
        Some(Doc::Object { items, keychain, .. }) => (items, keychain),
        Some(Doc::Null) => return Err(type_error("Cannot read properties of null (reading 'items')")),
        Some(_) => (None, None),
    };
    // for (const it of customCosmetics.items || []) if (it?.templateId && !cosmetics.items.some(same id)) push
    let custom: Vec<Item> = match citems {
        None | Some(Items::Null) => Vec::new(),
        Some(Items::Array(v)) => v,
        Some(Items::Primitive(v)) if !js::truthy(Some(&v)) || v.is_string() => Vec::new(),
        Some(_) => return Err(not_iterable("customCosmetics.items")),
    };
    let mut known: Option<HashSet<String>> = None;
    for it in custom {
        let Item::Object { template_id: Some(tid), variants } = it else { continue };
        if !js::truthy(Some(&tid)) {
            continue;
        }
        let Items::Array(list) = &mut items else {
            return Err(type_error("cosmetics.items.some is not a function"));
        };
        if !list.is_empty() {
            if known.is_none() {
                let mut set = HashSet::with_capacity(list.len());
                for c in list.iter() {
                    set.insert(template_of(c, "c.templateId")?.to_lowercase());
                }
                known = Some(set);
            }
            let Value::String(t) = &tid else {
                return Err(type_error("it.templateId.toLowerCase is not a function"));
            };
            if known.as_ref().unwrap().contains(&t.to_lowercase()) {
                continue;
            }
        }
        if let (Some(set), Value::String(t)) = (known.as_mut(), &tid) {
            set.insert(t.to_lowercase());
        }
        // { variants: [], ...it }
        list.push(Item::Object { template_id: Some(tid), variants });
    }
    crate::trace::mark("custom items merged");

    // for (const it of cosmetics.items || []) variantsOf.set(it.templateId.toLowerCase(), it.variants || [])
    let list = match items {
        Items::Array(v) => v,
        Items::Primitive(v) if !js::truthy(Some(&v)) => Vec::new(),
        Items::Primitive(Value::String(_)) => return Err(type_error("Cannot read properties of undefined (reading 'toLowerCase')")),
        _ => return Err(not_iterable("cosmetics.items")),
    };
    let count = list.len();
    let mut variants: HashMap<String, Box<str>> = HashMap::with_capacity(count);
    // items[id] = ... in order: first position, last value; V8 puts array-index keys first
    let mut order: Vec<(String, String)> = Vec::with_capacity(count);
    let mut at: HashMap<String, usize> = HashMap::with_capacity(count);
    for it in list {
        let tid = template_of(&it, "it.templateId")?;
        let id = tid.to_lowercase();
        let Item::Object { variants: v, .. } = it else { unreachable!() };
        variants.insert(id.clone(), v.unwrap_or_else(|| "[]".into()));
        match at.get(&id) {
            Some(&i) => order[i].1 = tid,
            None => {
                at.insert(id.clone(), order.len());
                order.push((id, tid));
            }
        }
    }
    let (mut index_keys, rest): (Vec<_>, Vec<_>) = order.into_iter().partition(|(id, _)| js::is_index_key(id));
    index_keys.sort_by_key(|(id, _)| id.parse::<u64>().unwrap_or(0));
    let mut athena_keys: Vec<AthenaKey> = index_keys.into_iter().chain(rest).map(|(id, tid)| AthenaKey::Cosmetic(id, tid)).collect();
    // items[id] ??= default; items['sandbox_loadout'] = locker
    for (_, d) in athena_defaults {
        let k = d.to_lowercase();
        if !athena_keys.iter().any(|a| matches!(a, AthenaKey::Cosmetic(id, _) if *id == k)) {
            athena_keys.push(AthenaKey::Default(d));
        }
    }
    match athena_keys.iter().position(|a| matches!(a, AthenaKey::Cosmetic(id, _) if id == "sandbox_loadout")) {
        Some(i) => athena_keys[i] = AthenaKey::Sandbox,
        None => athena_keys.push(AthenaKey::Sandbox),
    }
    crate::trace::mark("variants indexed");
    let mut source = js::or(source, Value::String("none".into()));
    js::normalize_value(&mut source);
    let mut keychain = keychain;
    if let Some(k) = keychain.as_mut() {
        js::normalize_value(k);
    }
    Ok(Cosmetics { count, source, keychain, variants, athena_keys })
}

#[cfg(test)]
mod tests {
    use super::*;

    const DEFAULTS: [(&str, &str); 2] = [("Character", "AthenaCharacter:CID_Default"), ("Pickaxe", "AthenaPickaxe:DefaultPickaxe")];

    fn dir(cos: Option<&str>, custom: Option<&str>) -> std::path::PathBuf {
        static N: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);
        let d = std::env::temp_dir().join(format!("rb-cos-{}-{}", std::process::id(), N.fetch_add(1, std::sync::atomic::Ordering::SeqCst)));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        if let Some(c) = cos {
            std::fs::write(d.join("cosmetics.json"), c).unwrap();
        }
        if let Some(c) = custom {
            std::fs::write(d.join("custom-cosmetics.json"), c).unwrap();
        }
        d
    }

    fn keys(c: &Cosmetics) -> Vec<String> {
        c.athena_keys
            .iter()
            .map(|k| match k {
                AthenaKey::Cosmetic(id, tid) => format!("{}={}", id, tid),
                AthenaKey::Default(t) => format!("default:{}", t),
                AthenaKey::Sandbox => "sandbox".into(),
            })
            .collect()
    }

    #[test]
    fn invalid_or_missing_files_are_the_defaults() {
        let d = dir(Some("{\"items\":[{\"templateId\":\"A:X\"}], oops"), Some("{\"items\":[{\"templateId\":\"B:Y\"}]} trailing"));
        let c = load(&d, &DEFAULTS).unwrap();
        assert_eq!((c.count, c.keychain.clone()), (0, Some(serde_json::json!([]))));
        assert_eq!(keys(&c), ["default:AthenaCharacter:CID_Default", "default:AthenaPickaxe:DefaultPickaxe", "sandbox"]);
        let _ = std::fs::remove_dir_all(&d);
    }

    fn scan(text: &[u8]) -> Option<Doc> {
        Scanner::new(text).doc().ok()
    }

    #[test]
    fn scanner_agrees_with_serde_on_random_documents() {
        // small random documents built from JSON tokens: every one must be judged the same way as serde_json does
        let toks: [&[u8]; 22] = [b"{", b"}", b"[", b"]", b":", b",", b"\"k\"", b"\"items\"", b"1", b"-2.5e3", b"0", b"01", b"true", b"null",
            b" ", b"\"\\n\"", b"\"\\q\"", b"x", b"\"templateId\"", b"\"variants\"", b".", b"\n"];
        let mut seed: u64 = 0x9E3779B97F4A7C15;
        for _ in 0..20000 {
            let mut doc = Vec::new();
            seed ^= seed << 13;
            seed ^= seed >> 7;
            seed ^= seed << 17;
            let n = 1 + (seed % 12) as usize;
            for i in 0..n {
                let r = seed.rotate_left(i as u32 * 5);
                doc.extend_from_slice(toks[(r % toks.len() as u64) as usize]);
            }
            let serde_ok = serde_json::from_slice::<serde_json::Value>(&doc).is_ok();
            assert_eq!(scan(&doc).is_some(), serde_ok, "{}", String::from_utf8_lossy(&doc));
        }
    }

    #[test]
    fn small_reads_do_not_change_the_result() {
        // a reader returning one byte at a time exercises every refill boundary
        struct OneByte<'a>(&'a [u8]);
        impl std::io::Read for OneByte<'_> {
            fn read(&mut self, b: &mut [u8]) -> std::io::Result<usize> {
                if self.0.is_empty() || b.is_empty() {
                    return Ok(0);
                }
                b[0] = self.0[0];
                self.0 = &self.0[1..];
                Ok(1)
            }
        }
        let text = b"{\"source\":\"s\\u00e9\",\"items\":[{\"templateId\":\"A:B\",\"variants\":[{\"channel\":\"x\",\"n\":-1.5e2}],\"names\":[\"a\",\"b\"]},null,\"s\"],\"keychain\":[\"k\"]}";
        let Some(Doc::Object { items: Some(Items::Array(v)), source, keychain }) = Scanner::new(OneByte(text)).doc().ok() else { panic!() };
        assert_eq!(source, Some(Value::String("sé".into())));
        assert_eq!(keychain, Some(serde_json::json!(["k"])));
        assert!(matches!(&v[0], Item::Object { template_id: Some(Value::String(t)), variants: Some(_) } if t == "A:B"));
        assert!(matches!(v[1], Item::Null) && matches!(v[2], Item::Other));
    }
}
