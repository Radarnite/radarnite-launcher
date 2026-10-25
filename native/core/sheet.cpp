#include "sheet.h"
#include <math.h>
#include <stdio.h>
#include <algorithm>

namespace rn {
namespace sheet {
namespace {

const wchar_t* kMon[] = { L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun", L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec" };
const wchar_t* kMonth[] = { L"January", L"February", L"March", L"April", L"May", L"June", L"July", L"August", L"September",
                            L"October", L"November", L"December" };

// Howard Hinnant's days_from_civil / civil_from_days
int fromCivil(int y, int m, int d) {
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

// stayList(): the stays, newest first; in the shop today the current one is extended to today (or opened)
std::vector<PortStay> stayList(const PortPreview& p, bool inShop) {
    std::vector<PortStay> s;
    for (auto& x : p.stays) if (x.from) s.push_back({ x.from, x.to ? x.to : x.from });
    if (inShop && p.today) {
        int y = p.today - 1;
        if (!s.empty() && s[0].to >= y) s[0].to = std::max(s[0].to, p.today);
        else s.insert(s.begin(), { p.today, p.today });
    }
    return s;
}

std::wstring fmtDay(double t) { return date((int)floor(t)); }

} // namespace

int dayOf(const char* s, size_t n) {
    if (!s || n < 10 || s[4] != '-' || s[7] != '-') return 0;
    int v[3] = {}, at[3] = { 0, 5, 8 }, len[3] = { 4, 2, 2 };
    for (int k = 0; k < 3; k++)
        for (int i = 0; i < len[k]; i++) {
            char c = s[at[k] + i];
            if (c < '0' || c > '9') return 0;
            v[k] = v[k] * 10 + (c - '0');
        }
    if (v[1] < 1 || v[1] > 12 || v[2] < 1 || v[2] > 31) return 0;
    return fromCivil(v[0], v[1], v[2]);
}

void civil(int z, int* y, int* m, int* d) {
    z += 719468;
    int era = (z >= 0 ? z : z - 146096) / 146097;
    int doe = z - era * 146097;
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = yoe + era * 400 + (*m <= 2);
}

int weekday(int day) { return ((day + 3) % 7 + 7) % 7; }   // 1970-01-01 was a Thursday
int monthOf(int day) { int y, m, d; civil(day, &y, &m, &d); return y * 12 + m - 1; }
int monthStart(int mi) { return fromCivil(mi / 12, mi % 12 + 1, 1); }
int monthDays(int mi) { return monthStart(mi + 1) - monthStart(mi); }

std::wstring num(int n) {
    std::wstring s = std::to_wstring(n < 0 ? -n : n), o;
    int c = 0;
    for (int i = (int)s.size() - 1; i >= 0; i--) { o.insert(o.begin(), s[i]); if (++c % 3 == 0 && i > 0) o.insert(o.begin(), L','); }
    return n < 0 ? L"-" + o : o;
}

std::wstring date(int day) {
    int y, m, d; civil(day, &y, &m, &d);
    return std::wstring(kMon[m - 1]) + L" " + std::to_wstring(d) + L", " + std::to_wstring(y);
}

std::wstring range(int a, int b) {
    int ya, ma, da, yb, mb, db;
    civil(a, &ya, &ma, &da); civil(b, &yb, &mb, &db);
    if (a == b) return date(a);
    if (ya != yb) return date(a) + L" – " + date(b);
    if (ma != mb) return std::wstring(kMon[ma - 1]) + L" " + std::to_wstring(da) + L" – " + date(b);
    return std::wstring(kMon[ma - 1]) + L" " + std::to_wstring(da) + L" – " + std::to_wstring(db) + L", " + std::to_wstring(yb);
}

std::wstring month(int mi) { return std::wstring(kMonth[mi % 12]) + L" " + std::to_wstring(mi / 12); }
std::wstring days(int n) { return num(n) + (n == 1 ? L" day" : L" days"); }

std::wstring rel(int n) {
    if (n == 0) return L"today";
    if (n == 1) return L"tomorrow";
    if (n == -1) return L"yesterday";
    return n > 0 ? L"in " + days(n) : days(-n) + L" ago";
}

std::wstring ago(int today, int day) { int n = today - day; return n <= 0 ? L"Today" : rel(-n); }

std::wstring pct(int pm) {
    double v = pm / 10.0;
    if (v > 0 && v < .5) return L"<1%";
    return L"~" + std::to_wstring((int)floor(v + .5)) + L"%";
}

int estPrice(PortType t, bool series, const std::string& r) {
    struct P { int uncommon, rare, epic, legendary, series; };
    static const P outfit = { 800, 1200, 1500, 2000, 1500 }, emote = { 200, 500, 800, 800, 500 },
                   pickaxe = { 500, 800, 1200, 1500, 800 }, glider = { 500, 800, 1200, 1500, 1200 };
    const P* p = t == PortType::Outfit ? &outfit : t == PortType::Emote ? &emote : t == PortType::Pickaxe ? &pickaxe : t == PortType::Glider ? &glider : nullptr;
    if (!p) return 0;
    if (series) return p->series;
    return r == "uncommon" ? p->uncommon : r == "rare" ? p->rare : r == "epic" ? p->epic : r == "legendary" ? p->legendary : 0;
}

Sheet build(const PortPreview& p, PortType type, bool series, const std::string& rarity) {
    Sheet s;
    int today = p.today;
    s.today = today;
    bool inShop = p.inShop;
    s.inShop = inShop;
    bool noHist = p.stays.empty() && !p.first && p.occ <= 0;
    s.source = inShop || !noHist;
    if (p.price > 0) { s.price = p.price; s.regular = p.regular; }
    else s.price = estPrice(type, series, rarity);
    // Chance of returning (catalog p7/p30/p60), only while away
    if (p.p7 >= 0 && !inShop) {
        s.chance = true;
        s.chances[0] = pct(p.p7); s.chances[1] = pct(p.p30 >= 0 ? p.p30 : 0); s.chances[2] = pct(p.p60 >= 0 ? p.p60 : 0);
    }
    if (noHist || !today) return s;

    // ---- Shop history (histHtml)
    std::vector<PortStay> S = stayList(p, inShop);
    int n = (int)S.size();
    int relD = n ? (p.first && p.first < S[n - 1].from ? p.first : S[n - 1].from) : p.first;
    if (relD) s.cells.push_back({ L"First released", ago(today, relD), date(relD) });
    {   // Last seen
        std::vector<PortStay> S0 = stayList(p, false);
        if (inShop) s.cells.push_back({ L"Last seen", L"Today", L"" });
        else {
            int d = !S0.empty() ? S0[0].to : p.last;
            if (d) s.cells.push_back({ L"Last seen", ago(today, d), date(d) });
        }
    }
    if (n >= 2) {   // median between the starts of consecutive stays
        std::vector<int> g;
        for (int i = 0; i < n - 1; i++) g.push_back(S[i].from - S[i + 1].from);
        std::sort(g.begin(), g.end());
        size_t L = g.size();
        int m = L % 2 ? g[(L - 1) / 2] : (int)floor((g[L / 2 - 1] + g[L / 2]) / 2.0 + .5);
        if (m > 0) s.cells.push_back({ L"Usually returns every", days(m), L"based on " + num(n - 1) + (n - 1 == 1 ? L" return" : L" returns") });
    }
    // ---- Return prediction (retHtml)
    s.ret = true;
    std::vector<int> gaps;
    for (int i = 0; i < n - 1; i++) { int g = S[i].from - S[i + 1].to - 1; if (g > 0) gaps.push_back(g); }
    const wchar_t* unc = L"Predictions are uncertain; this is not a confirmed shop date.";
    if (n < 2 || gaps.empty()) { s.retDate = L"Not enough history"; s.retSub = L"Needs at least two shop appearances"; }
    else {
        std::sort(gaps.begin(), gaps.end());
        int end = inShop ? today : S[0].to;
        double e0 = end + .5, t0 = today + .5;
        int away = inShop ? 0 : std::max(0, today - end);
        std::vector<int> L;
        for (int g : gaps) if (g > away) L.push_back(g);
        auto q = [&](double x) { int k = (int)floor(x * (L.size() - 1) + .5); return L[std::min((int)L.size() - 1, std::max(0, k))]; };
        if (L.empty()) {
            s.retDate = L"Overdue";
            s.retSub = L"Away " + days(away) + L", longer than ever before";
            s.retInfo = L"Its longest previous absence was " + days(gaps.back()) + L", so its history can’t place a date. " + unc;
        } else {
            double a = e0 + q(.25), b = e0 + q(.75);
            if (b - a < 14) { double c = (a + b) / 2; a = c - 7; b = c + 7; }
            if (a <= t0) a = t0 + 1;
            if (b < a + 7) b = a + 7;
            // the day of most density of those absences (+-1 week) between tomorrow and 15 days after the window
            double mx = 0, pk = -1;
            for (double t = t0 + 1; t <= b + 15; t += 1) {
                double v = 0;
                for (int g : L) { double z = ((t - e0) - g) / 7; v += exp(-.5 * z * z); }
                if (v > mx) { mx = v; pk = t; }
            }
            int A = (int)floor(a), B = (int)floor(b);
            auto dn = [&](double x) { return L"(" + rel((int)floor(x - t0 + .5)) + L")"; };
            if (pk >= 0) {
                s.retDate = fmtDay(pk); s.retDays = dn(pk);
                s.retSub = (int)floor(pk) == A ? L"Likely window through " + date(B) : L"Likely window " + range(A, B);
            } else { s.retDate = range(A, B); s.retDays = dn(a); }
            std::wstring basis = inShop ? L"Based on " + num((int)gaps.size()) + ((int)gaps.size() == 1 ? L" previous return interval." : L" previous return intervals.")
                                        : L"Based on " + num((int)L.size()) + ((int)L.size() == 1 ? L" previous return interval" : L" previous return intervals") +
                                              L" longer than its current absence (" + days(away) + L").";
            s.retInfo = basis + L" " + unc;
            s.cal = true;
            s.a = A; s.b = B; s.peak = pk >= 0 ? (int)floor(pk) : 0;
            s.lo = monthOf(today);
            s.hi = std::max(monthOf(B), s.peak ? monthOf(s.peak) : s.lo);
            s.m = std::min(s.hi, std::max(s.lo, s.peak ? monthOf(s.peak) : monthOf(A)));
        }
    }
    s.history = true;

    // ---- Appearance history (tlineHtml)
    for (int k = 0; k < n; k++) {
        const PortStay& x = S[k];
        Stay st;
        st.cur = k == 0 && inShop;
        st.date = date(x.from);
        st.label = st.cur ? (n == 1 ? L"Current shop stay (first appearance)" : L"Current shop stay")
                 : k == n - 1 && n > 1 ? L"First appearance" : k == 0 ? L"Last appearance" : L"Previous appearance";
        int last = st.cur ? today : x.to, count = 0;
        for (int d = x.from; d <= last && count < 400; d++, count++)
            if (count < 14) st.days.push_back({ date(d), ago(today, d) });
        st.more = std::max(0, count - 14);
        s.stays.push_back(std::move(st));
    }
    return s;
}

} // namespace sheet
} // namespace rn
