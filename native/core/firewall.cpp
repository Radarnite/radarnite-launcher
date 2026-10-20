#include "firewall.h"
#include "paths.h"
#include "util.h"
#include <objbase.h>
#include <oleauto.h>
#include <netfw.h>
#include <deque>

namespace rn {
namespace firewall {

const wchar_t* const kNonLoopback = L"0.0.0.0-126.255.255.255,128.0.0.0-255.255.255.255,::-::0,::2-ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff";

std::wstring group() {
    std::wstring g = getEnv(L"RADARNITE_FW_GROUP");
    return g.empty() ? L"FN Offline Builds" : g;
}

std::vector<std::wstring> programsFor(const std::wstring& build) {
    std::vector<std::wstring> found;
    // Directory.EnumerateFiles(build, "*.exe", AllDirectories): a folder's files in directory order, its subfolders
    // queued and walked after it (breadth-first)
    std::deque<std::wstring> pending{ build };
    while (!pending.empty()) {
        std::wstring d = pending.front();
        pending.pop_front();
        std::wstring base = !d.empty() && (d.back() == L'\\' || d.back() == L'/') ? d : d + L"\\";
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW((base + L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) pending.push_back(base + fd.cFileName);
            } else if (iends(fd.cFileName, L".exe")) {
                found.push_back(base + fd.cFileName);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    found.push_back(paths::node());
    found.push_back(paths::backend());
    found.push_back(paths::cosmeticsGen());
    std::vector<std::wstring> out;
    std::set<std::wstring> seen;
    for (auto& f : found) {
        if (!fileExists(f)) continue;
        std::wstring full = fullPath(f);
        if (seen.insert(upper(full)).second) out.push_back(full);
    }
    return out;
}

namespace {

const LONG OUT_DIR = NET_FW_RULE_DIR_OUT, IN_DIR = NET_FW_RULE_DIR_IN;

template <typename T> struct Com {
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() { if (p) p->Release(); }
    T* operator->() const { return p; }
    T** put() { if (p) { p->Release(); p = nullptr; } return &p; }
};

struct Bstr {
    BSTR b = nullptr;
    Bstr() = default;
    explicit Bstr(const std::wstring& s) : b(SysAllocStringLen(s.data(), (UINT)s.size())) {}
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    ~Bstr() { SysFreeString(b); }
    BSTR* put() { SysFreeString(b); b = nullptr; return &b; }
    bool null() const { return b == nullptr; }
    std::wstring str() const { return b ? std::wstring(b, SysStringLen(b)) : L""; }
    bool is(const std::wstring& s) const { return b && SysStringLen(b) == s.size() && !wmemcmp(b, s.data(), s.size()); }
};

std::wstring hrText(HRESULT hr) {
    std::wstring m = winError((DWORD)hr);
    return m + fmt(L" (0x%08lX)", (unsigned long)hr);
}

// One COM apartment for the call (no-op when the thread already has one).
struct ComInit {
    HRESULT hr;
    ComInit() : hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ComInit() { if (SUCCEEDED(hr)) CoUninitialize(); }
};

HRESULT policy(INetFwPolicy2** pol) {
    CLSID clsid;
    HRESULT hr = CLSIDFromProgID(L"HNetCfg.FwPolicy2", &clsid);
    if (FAILED(hr)) return hr;
    return CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, __uuidof(INetFwPolicy2), (void**)pol);
}

// Calls f(rule) for every rule of the group, in one pass over the rule list; the other groups' rules only cost one
// get_Grouping each.
template <typename F>
HRESULT forGroup(INetFwRules* rules, const std::wstring& grp, F f) {
    Com<IUnknown> unk;
    HRESULT hr = rules->get__NewEnum(unk.put());
    if (FAILED(hr)) return hr;
    Com<IEnumVARIANT> en;
    hr = unk->QueryInterface(IID_IEnumVARIANT, (void**)en.put());
    if (FAILED(hr)) return hr;
    VARIANT v[64];
    for (;;) {
        ULONG got = 0;
        for (auto& x : v) VariantInit(&x);
        hr = en->Next(64, v, &got);
        if (FAILED(hr)) return hr;
        for (ULONG i = 0; i < got; i++) {
            if (v[i].vt == VT_DISPATCH && v[i].pdispVal) {
                Com<INetFwRule> rule;
                if (SUCCEEDED(v[i].pdispVal->QueryInterface(__uuidof(INetFwRule), (void**)rule.put()))) {
                    Bstr g;
                    if (SUCCEEDED(rule->get_Grouping(g.put())) && g.is(grp)) f(rule.p);
                }
            }
            VariantClear(&v[i]);
        }
        if (hr != S_OK || got == 0) return S_OK;
    }
}

} // namespace

Rules read() {
    Rules r;
    ComInit com;
    Com<INetFwPolicy2> pol;
    HRESULT hr = policy(pol.put());
    if (FAILED(hr)) { r.error = hrText(hr); return r; }
    LONG current = 0;
    hr = pol->get_CurrentProfileTypes(&current);
    if (FAILED(hr)) { r.error = hrText(hr); return r; }
    for (LONG p : { 1L, 2L, 4L }) {
        if (!(current & p)) continue;
        VARIANT_BOOL on = VARIANT_FALSE;
        hr = pol->get_FirewallEnabled((NET_FW_PROFILE_TYPE2)p, &on);
        if (FAILED(hr)) { r.error = hrText(hr); return r; }
        if (!on) { r.off = L"Windows Firewall is off for the current network profile"; return r; }
    }
    Com<INetFwRules> rules;
    hr = pol->get_Rules(rules.put());
    if (FAILED(hr)) { r.error = hrText(hr); return r; }
    hr = forGroup(rules.p, group(), [&](INetFwRule* rule) {
        VARIANT_BOOL enabled = VARIANT_FALSE;
        NET_FW_ACTION action = NET_FW_ACTION_ALLOW;
        NET_FW_RULE_DIRECTION dir = NET_FW_RULE_DIR_IN;
        if (FAILED(rule->get_Enabled(&enabled)) || !enabled) return;
        if (FAILED(rule->get_Action(&action)) || action != NET_FW_ACTION_BLOCK) return;
        if (FAILED(rule->get_Direction(&dir)) || dir != OUT_DIR) return;
        Bstr ra, app;
        if (FAILED(rule->get_RemoteAddresses(ra.put())) || ra.null() || contains(ra.str(), L"127.0.0.1")) return;
        if (FAILED(rule->get_ApplicationName(app.put())) || app.null()) return;
        r.blocked.insert(upper(fullPath(app.str())));
    });
    if (FAILED(hr)) r.error = hrText(hr);
    return r;
}

std::wstring check(const Rules& r, const std::wstring& build) {
    if (!r.error.empty()) return r.error;
    if (!r.off.empty()) return r.off;
    std::vector<std::wstring> missing;
    for (auto& p : programsFor(build)) if (!r.blocked.count(upper(p))) missing.push_back(p);
    if (missing.empty()) return L"";
    return fmt(L"firewall rules missing for %d programs (e.g. %ls)", (int)missing.size(), fileName(missing[0]).c_str());
}

std::wstring check(const std::wstring& build) { return check(read(), build); }

int create(const std::wstring& build, std::wstring* error) {
    ComInit com;
    Com<INetFwPolicy2> pol;
    HRESULT hr = policy(pol.put());
    if (FAILED(hr)) { if (error) *error = hrText(hr); return 0; }
    Com<INetFwRules> rules;
    hr = pol->get_Rules(rules.put());
    if (FAILED(hr)) { if (error) *error = hrText(hr); return 0; }
    std::wstring grp = group();
    auto programs = programsFor(build);
    std::set<std::wstring> keys;
    for (auto& p : programs) keys.insert(upper(p));
    // stale rules of the group for these programs (other names, e.g. a launcher folder that was renamed)
    std::vector<std::wstring> stale;
    forGroup(rules.p, grp, [&](INetFwRule* rule) {
        Bstr app, name;
        if (FAILED(rule->get_ApplicationName(app.put())) || app.null()) return;
        if (!keys.count(upper(fullPath(app.str())))) return;
        if (SUCCEEDED(rule->get_Name(name.put())) && !name.null()) stale.push_back(name.str());
    });
    for (auto& n : stale) { Bstr b(n); rules->Remove(b.b); }

    CLSID ruleClsid;
    hr = CLSIDFromProgID(L"HNetCfg.FWRule", &ruleClsid);
    if (FAILED(hr)) { if (error) *error = hrText(hr); return 0; }
    const std::wstring& ldir = paths::launcherDir();
    std::wstring root = paths::root();
    Bstr desc(L"Fortnite Offline: 127.0.0.1 only. Created by Radarnite Launcher (" + ldir + L").");
    Bstr remote(kNonLoopback), grpB(grp);
    int n = 0;
    for (auto& exe : programs) {
        std::wstring rel = isUnder(exe, ldir) ? join(fileName(ldir), relativePath(ldir, exe)) : relativePath(root, exe);
        Bstr app(exe);
        for (int k = 0; k < 2; k++) {
            LONG dir = k == 0 ? OUT_DIR : IN_DIR;
            Bstr name(L"FN Offline - " + rel + (k == 0 ? L" (outbound)" : L" (inbound)"));
            rules->Remove(name.b);
            Com<INetFwRule> rule;
            hr = CoCreateInstance(ruleClsid, nullptr, CLSCTX_INPROC_SERVER, __uuidof(INetFwRule), (void**)rule.put());
            if (SUCCEEDED(hr)) hr = rule->put_Name(name.b);
            if (SUCCEEDED(hr)) hr = rule->put_Description(desc.b);
            if (SUCCEEDED(hr)) hr = rule->put_ApplicationName(app.b);
            if (SUCCEEDED(hr)) hr = rule->put_Protocol(256);   // any protocol
            if (SUCCEEDED(hr)) hr = rule->put_RemoteAddresses(remote.b);
            if (SUCCEEDED(hr)) hr = rule->put_Direction((NET_FW_RULE_DIRECTION)dir);
            if (SUCCEEDED(hr)) hr = rule->put_Action(NET_FW_ACTION_BLOCK);
            if (SUCCEEDED(hr)) hr = rule->put_Profiles(NET_FW_PROFILE2_ALL);
            if (SUCCEEDED(hr)) hr = rule->put_Grouping(grpB.b);
            if (SUCCEEDED(hr)) hr = rule->put_Enabled(VARIANT_TRUE);
            if (SUCCEEDED(hr)) hr = rules->Add(rule.p);
            if (FAILED(hr)) { if (error) *error = hrText(hr); return n; }
            n++;
        }
    }
    return n;
}

int removeAll(std::wstring* error) {
    ComInit com;
    Com<INetFwPolicy2> pol;
    HRESULT hr = policy(pol.put());
    if (FAILED(hr)) { if (error) *error = hrText(hr); return 0; }
    Com<INetFwRules> rules;
    hr = pol->get_Rules(rules.put());
    if (FAILED(hr)) { if (error) *error = hrText(hr); return 0; }
    std::vector<std::wstring> names;
    hr = forGroup(rules.p, group(), [&](INetFwRule* rule) {
        Bstr name;
        if (SUCCEEDED(rule->get_Name(name.put())) && !name.null()) names.push_back(name.str());
    });
    if (FAILED(hr) && error) *error = hrText(hr);
    for (auto& n : names) { Bstr b(n); rules->Remove(b.b); }
    return (int)names.size();
}

} // namespace firewall
} // namespace rn
