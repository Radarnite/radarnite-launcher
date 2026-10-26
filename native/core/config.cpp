#include "config.h"
#include "json.h"
#include "paths.h"
#include "util.h"
#include <bcrypt.h>
#include <objbase.h>

namespace rn {
namespace {

enum Kind { Str, Int, Bool };
struct Field { const char* key; Kind kind; std::wstring LauncherConfig::*s; int LauncherConfig::*i; bool LauncherConfig::*b; };

// config.json order of the 2.x Config properties (System.Text.Json writes them in declaration order).
const Field kFields[] = {
    { "displayName", Str, &LauncherConfig::displayName, nullptr, nullptr },
    { "accountId", Str, &LauncherConfig::accountId, nullptr, nullptr },
    { "port", Int, nullptr, &LauncherConfig::port, nullptr },
    { "mode", Str, &LauncherConfig::mode, nullptr, nullptr },
    { "lastPlaylist", Str, &LauncherConfig::lastPlaylist, nullptr, nullptr },
    { "bots", Int, nullptr, &LauncherConfig::bots, nullptr },
    { "infiniteMaterials", Bool, nullptr, nullptr, &LauncherConfig::infiniteMaterials },
    { "infiniteAmmo", Bool, nullptr, nullptr, &LauncherConfig::infiniteAmmo },
    { "lateGame", Bool, nullptr, nullptr, &LauncherConfig::lateGame },
    { "practicePlaylist", Str, &LauncherConfig::practicePlaylist, nullptr, nullptr },
    { "matchPlaylist", Str, &LauncherConfig::matchPlaylist, nullptr, nullptr },
    { "botsAI", Bool, nullptr, nullptr, &LauncherConfig::botsAI },
    { "autoStartSeconds", Int, nullptr, &LauncherConfig::autoStartSeconds, nullptr },
    { "buildsFolder", Str, &LauncherConfig::buildsFolder, nullptr, nullptr },
};
const int kFieldCount = (int)(sizeof kFields / sizeof kFields[0]);

// Config.HelpText(): written into config.json as "_help" (JSON has no comments); rewritten on every save.
const char* const kHelp[][2] = {
    { "displayName", "Your in-game name (random by default)." },
    { "accountId", "Local account id (random; nothing to do with an Epic account)." },
    { "port", "Port of the local backend on 127.0.0.1." },
    { "mode", "Not used any more: pick the mode in the game (lobby -> CHANGE): Creative/Playground = practice alone with infinite materials and ammo; Solo, Duos, Trios, Squads = Battle Royale with bots." },
    { "lastPlaylist", "Last mode picked in the game; its match server is prepared when the game starts." },
    { "bots", "Bots that fill the bus in Solo/Duos/Trios/Squads (0-99)." },
    { "botsAI", "false (default) = bots without AI (they drop from the bus and land); true = the game's own bot AI (Season 11+), EXPERIMENTAL: on 17.50 it crashes the match server a few seconds after it starts." },
    { "infiniteMaterials", "Infinite building materials in Battle Royale matches (practice always has them)." },
    { "infiniteAmmo", "Infinite ammo in Battle Royale matches (practice always has it)." },
    { "lateGame", "Start in late game (small storm, shields)." },
    { "practicePlaylist", "Playlist the match server runs when you pick Creative/Playground (practice)." },
    { "matchPlaylist", "Not used any more (the Battle Royale playlist is the one picked in the game)." },
    { "autoStartSeconds", "Seconds after you connect until the bus leaves by itself (0 = it does not leave by itself)." },
    { "buildsFolder", "Folder that holds your Fortnite builds, one folder per build (empty = the folder that contains the launcher's folder)." },
};
const int kHelpCount = (int)(sizeof kHelp / sizeof kHelp[0]);

// A System.Text.Json Int32: an integer token (no fraction/exponent) inside the int range.
bool parseInt32(const char* s, size_t n, int* out) {
    size_t i = 0;
    bool neg = n > 0 && s[0] == '-';
    if (neg) i++;
    if (i >= n) return false;
    long long v = 0;
    for (; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + (s[i] - '0');
        if (v > 2147483648ll) return false;
    }
    if (neg) v = -v;
    if (v > 2147483647ll || v < -2147483648ll) return false;
    *out = (int)v;
    return true;
}

// JsonSerializer.Deserialize<Config> with PropertyNameCaseInsensitive: false = it would have thrown.
bool parseInto(LauncherConfig& c, const std::string& text) {
    yyjson_doc* doc = json::parse(text, YYJSON_READ_NUMBER_AS_RAW);
    if (!doc) return false;
    yyjson_val* root = yyjson_doc_get_root(doc);
    bool ok = true;
    if (yyjson_is_null(root)) { yyjson_doc_free(doc); return true; }   // "null" -> new Config()
    if (!yyjson_is_obj(root)) { yyjson_doc_free(doc); return false; }
    size_t idx, max;
    yyjson_val *k, *v;
    yyjson_obj_foreach(root, idx, max, k, v) {
        std::wstring key = json::wstr(k);
        if (ieq(key, L"_help")) {
            if (yyjson_is_null(v)) { c.helpCount = -1; continue; }
            if (!yyjson_is_obj(v)) { ok = false; break; }
            size_t hi, hm;
            yyjson_val *hk, *hv;
            yyjson_obj_foreach(v, hi, hm, hk, hv) if (!yyjson_is_str(hv) && !yyjson_is_null(hv)) ok = false;
            c.helpCount = (int)yyjson_obj_size(v);
            if (!ok) break;
            continue;
        }
        int f = -1;
        for (int i = 0; i < kFieldCount && f < 0; i++) if (ieq(key, widen(kFields[i].key))) f = i;
        if (f < 0) { c.extra.emplace_back(std::string(yyjson_get_str(k), yyjson_get_len(k)), json::indented(v, 1)); continue; }
        const Field& fd = kFields[f];
        if (fd.kind == Str) {
            if (yyjson_is_null(v)) { (c.*fd.s).clear(); c.nulls |= 1u << f; }
            else if (yyjson_is_str(v)) { c.*fd.s = json::wstr(v); c.nulls &= ~(1u << f); }
            else { ok = false; break; }
        } else if (fd.kind == Int) {
            if (!yyjson_is_raw(v) || !parseInt32(yyjson_get_raw(v), yyjson_get_len(v), &(c.*fd.i))) { ok = false; break; }
        } else {
            if (!yyjson_is_bool(v)) { ok = false; break; }
            c.*fd.b = yyjson_get_bool(v);
        }
    }
    yyjson_doc_free(doc);
    return ok;
}

// The first match of "displayName"\s*:\s*"([^"]{1,32})" (or the accountId pattern) in the raw text.
std::wstring recover(const std::wstring& raw, const std::wstring& key, bool hex32) {
    std::wstring lit = L"\"" + key + L"\"";
    for (size_t p = raw.find(lit); p != std::wstring::npos; p = raw.find(lit, p + 1)) {
        size_t i = p + lit.size();
        while (i < raw.size() && isWhite(raw[i])) i++;
        if (i >= raw.size() || raw[i] != L':') continue;
        i++;
        while (i < raw.size() && isWhite(raw[i])) i++;
        if (i >= raw.size() || raw[i] != L'"') continue;
        size_t s = ++i;
        size_t e = raw.find(L'"', s);
        if (e == std::wstring::npos) continue;
        size_t n = e - s;
        if (hex32) {
            if (n != 32) continue;
            bool good = true;
            for (size_t j = s; j < e; j++) if (!((raw[j] >= L'0' && raw[j] <= L'9') || (raw[j] >= L'a' && raw[j] <= L'f'))) good = false;
            if (!good) continue;
        } else if (n < 1 || n > 32) continue;
        return raw.substr(s, n);
    }
    return L"";
}

// Regex.IsMatch(id, "^[0-9a-f]{32}$") ($ also matches before a final "\n").
bool validAccount(const std::wstring& id) {
    size_t n = id.size();
    if (n == 33 && id[32] == L'\n') n = 32;
    if (n != 32) return false;
    for (size_t i = 0; i < 32; i++) if (!((id[i] >= L'0' && id[i] <= L'9') || (id[i] >= L'a' && id[i] <= L'f'))) return false;
    return true;
}

unsigned rnd(unsigned n) {
    unsigned v = 0;
    BCryptGenRandom(nullptr, (PUCHAR)&v, sizeof v, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return v % n;
}

std::wstring newGuidN() {
    GUID g;
    CoCreateGuid(&g);
    return fmt(L"%08lx%04x%04x%02x%02x%02x%02x%02x%02x%02x%02x", (unsigned long)g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1],
               g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
}

} // namespace

LauncherConfig LauncherConfig::load() {
    paths::ConfigLock lock;
    LauncherConfig c;
    std::wstring path = paths::config();
    std::string text;
    DWORD e = 0;
    bool read = readFile(path, &text, &e);
    bool broken = read ? !parseInto(c, text) : e != ERROR_FILE_NOT_FOUND;
    if (broken) {
        // A config.json edited by hand with a mistake: keep the person's file (config.json.broken) instead of silently
        // replacing their name and account with new random ones, and start from the defaults.
        c = LauncherConfig();
        CopyFileW(path.c_str(), (path + L".broken").c_str(), FALSE);
        if (read) {
            std::wstring raw = widen(text);
            if (raw.size() && raw[0] == 0xFEFF) raw.erase(0, 1);
            c.displayName = recover(raw, L"displayName", false);
            c.accountId = recover(raw, L"accountId", true);
        }
    }
    bool changed = false;
    if (blank(c.displayName)) {
        static const wchar_t* a[] = { L"Swift", L"Tactical", L"Silent", L"Brave", L"Electric", L"Midnight", L"Golden", L"Fierce" };
        static const wchar_t* b[] = { L"Wolf", L"Hawk", L"Fox", L"Tiger", L"Raven", L"Lynx", L"Bear", L"Puma" };
        c.displayName = std::wstring(a[rnd(8)]) + b[rnd(8)] + std::to_wstring(10 + rnd(89));
        c.nulls &= ~1u;
        changed = true;
    }
    if (!validAccount(c.accountId)) { c.accountId = newGuidN(); c.nulls &= ~2u; changed = true; }
    if (changed || !fileExists(path) || c.helpCount != kHelpCount) { c.save(); c.helpCount = kHelpCount; }
    return c;
}

std::string LauncherConfig::serialize() const {
    std::string o = "{";
    bool first = true;
    auto member = [&](const std::string& key, const std::string& value) {
        o += first ? "\r\n  " : ",\r\n  ";
        first = false;
        o += key + ": " + value;
    };
    for (int i = 0; i < kFieldCount; i++) {
        const Field& f = kFields[i];
        std::string key = std::string("\"") + f.key + "\"";
        if (f.kind == Str) member(key, (nulls & (1u << i)) ? "null" : json::relaxed(this->*f.s));
        else if (f.kind == Int) member(key, std::to_string(this->*f.i));
        else member(key, (this->*f.b) ? "true" : "false");
    }
    for (auto& x : extra) member(json::relaxed(widen(x.first)), x.second);
    std::string help = "{";
    for (int i = 0; i < kHelpCount; i++)
        help += std::string(i ? ",\r\n    " : "\r\n    ") + "\"" + kHelp[i][0] + "\": " + json::relaxed(widen(kHelp[i][1]));
    help += "\r\n  }";
    member("\"_help\"", help);
    return o + "\r\n}";
}

std::wstring LauncherConfig::extraStr(const char* key) const {
    for (auto& x : extra) {
        if (x.first != key) continue;
        yyjson_doc* d = json::parse(x.second);
        std::wstring v = d ? json::wstr(yyjson_doc_get_root(d)) : L"";
        if (d) yyjson_doc_free(d);
        return v;
    }
    return L"";
}

int LauncherConfig::extraInt(const char* key, int def) const {
    for (auto& x : extra) {
        if (x.first != key) continue;
        yyjson_doc* d = json::parse(x.second);
        yyjson_val* v = d ? yyjson_doc_get_root(d) : nullptr;
        int r = v && yyjson_is_num(v) ? (int)yyjson_get_num(v) : def;
        if (d) yyjson_doc_free(d);
        return r;
    }
    return def;
}

void LauncherConfig::setExtraInt(const char* key, int value) {
    for (auto& x : extra) if (x.first == key) { x.second = std::to_string(value); return; }
    extra.emplace_back(key, std::to_string(value));
}

void LauncherConfig::setExtraStr(const char* key, const std::wstring& value) {
    for (auto& x : extra) if (x.first == key) { x.second = json::relaxed(value); return; }
    extra.emplace_back(key, json::relaxed(value));
}

bool LauncherConfig::save() const {
    paths::ConfigLock lock;
    bool ok = writeFile(paths::config(), serialize());
    paths::resetRoot();
    return ok;
}

static const wchar_t* kUiKey = L"Software\\Radarnite\\Launcher\\UI";

std::wstring uiPref(const wchar_t* key) {
    wchar_t buf[1024];
    DWORD size = sizeof buf;
    if (RegGetValueW(HKEY_CURRENT_USER, kUiKey, key, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return L"";
    return buf;
}

void setUiPref(const wchar_t* key, const std::wstring& value) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kUiKey, key, REG_SZ, value.c_str(), (DWORD)((value.size() + 1) * sizeof(wchar_t)));
}

} // namespace rn
