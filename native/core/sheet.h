// radarnite.com's item sheet (dashboard/index.html openItem: histHtml, retHtml, tlineHtml, the price and the chance of
// returning) computed from a PortPreview, in the site's English wording. Pure: no Windows, no I/O, so coretest checks
// it against what the site shows for the same data. Dates are days since 1970-01-01 (UTC), like the site's dates at
// noon UTC; "today" is the catalog's date, never the PC's clock.
#pragma once
#include "../ui/api.h"
#include <string>
#include <utility>
#include <vector>

namespace rn {
namespace sheet {

int dayOf(const char* s, size_t n);          // "YYYY-MM-DD..." -> day, 0 when it is not a date
void civil(int day, int* y, int* m, int* d); // m 1..12
int weekday(int day);                        // 0 Monday .. 6 Sunday
int monthOf(int day);                        // y * 12 + (m - 1)
int monthStart(int mi);                      // the day of its 1st
int monthDays(int mi);
std::wstring date(int day);                  // "Jul 17, 2024"
std::wstring range(int a, int b);            // Intl formatRange: "Jan 17 – Mar 23, 2027", "Oct 12 – 25, 2026"
std::wstring month(int mi);                  // "March 2027"
std::wstring num(int n);                     // "1,500"
std::wstring days(int n);                    // "1 day", "159 days"
std::wstring ago(int today, int day);        // "Today", "yesterday", "829 days ago"
std::wstring rel(int n);                     // Intl.RelativeTimeFormat numeric:auto: "tomorrow", "in 130 days"
std::wstring pct(int perMille);              // "<1%", "~4%"
int estPrice(PortType t, bool series, const std::string& rarity);   // the shop's usual price (PRICE_STD), 0 = none

struct Cell { std::wstring label, value, sub; };
struct Stay {
    std::wstring date, label;                // "Sep 26, 2026", "Last appearance"
    bool cur = false;                        // the current stay (filled dot)
    std::vector<std::pair<std::wstring, std::wstring>> days;   // its days (14 at most): "Sep 26, 2026", "28 days ago"
    int more = 0;                            // "+N more"
};
struct Sheet {
    bool inShop = false;
    bool source = false;                     // "Source Shop"
    int price = 0, regular = 0;              // V-Bucks (regular > price: struck through); 0 = no price
    bool history = false;                    // the Shop history section
    std::vector<Cell> cells;                 // First released, Last seen, Usually returns every
    bool ret = false;                        // the Return prediction card
    std::wstring retDate, retDays, retSub, retInfo;   // "Mar 3, 2027", "(in 130 days)", "Likely window ...", the (i) text
    bool cal = false;                        // its calendar: window [a, b], estimated day peak (0 none), months lo..hi, shown m
    int today = 0, a = 0, b = 0, peak = 0, lo = 0, hi = 0, m = 0;
    bool chance = false;                     // Chance of returning: 7, 30, 60 days
    std::wstring chances[3];
    std::vector<Stay> stays;                 // Appearance history, newest first
};
Sheet build(const PortPreview& p, PortType type, bool series, const std::string& rarity);

} // namespace sheet
} // namespace rn
