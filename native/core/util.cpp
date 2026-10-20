#include "util.h"
#include <objbase.h>
#include <shlobj.h>
#include <stdarg.h>
#include <stdio.h>
#include <wchar.h>

namespace rn {

// ------------------------------------------------------------------------------------------------ strings
std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring upper(const std::wstring& s) {
    if (s.empty()) return s;
    std::wstring u(s.size(), L'\0');
    int n = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, s.data(), (int)s.size(), &u[0], (int)u.size(), nullptr, nullptr, 0);
    if (n <= 0) return s;
    u.resize(n);
    return u;
}

bool ieq(const std::wstring& a, const std::wstring& b) {
    return a.size() == b.size() && CompareStringOrdinal(a.data(), (int)a.size(), b.data(), (int)b.size(), TRUE) == CSTR_EQUAL;
}

bool istarts(const std::wstring& s, const std::wstring& p) {
    return s.size() >= p.size() && CompareStringOrdinal(s.data(), (int)p.size(), p.data(), (int)p.size(), TRUE) == CSTR_EQUAL;
}

bool iends(const std::wstring& s, const std::wstring& p) {
    return s.size() >= p.size() && CompareStringOrdinal(s.data() + s.size() - p.size(), (int)p.size(), p.data(), (int)p.size(), TRUE) == CSTR_EQUAL;
}

bool starts(const std::wstring& s, const wchar_t* p) { return s.rfind(p, 0) == 0; }

bool contains(const std::wstring& s, const std::wstring& part) { return s.find(part) != std::wstring::npos; }

bool isWhite(wchar_t c) {
    if (c == L' ' || (c >= 0x09 && c <= 0x0D) || c == 0x85 || c == 0xA0) return true;
    return c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

std::wstring trim(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && isWhite(s[a])) a++;
    while (b > a && isWhite(s[b - 1])) b--;
    return s.substr(a, b - a);
}

bool blank(const std::wstring& s) {
    for (wchar_t c : s) if (!isWhite(c)) return false;
    return true;
}

std::wstring fmt(const wchar_t* f, ...) {
    wchar_t buf[2048];
    va_list a;
    va_start(a, f);
    int n = _vsnwprintf(buf, 2047, f, a);
    va_end(a);
    if (n < 0) n = 2047;
    buf[n] = 0;
    return buf;
}

std::wstring hex(const uint8_t* p, size_t n) {
    static const wchar_t* d = L"0123456789abcdef";
    std::wstring s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; i++) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
    return s;
}

// ------------------------------------------------------------------------------------------------ paths
static bool isSep(wchar_t c) { return c == L'\\' || c == L'/'; }

static bool rooted(const std::wstring& p) {
    return (!p.empty() && isSep(p[0])) || (p.size() >= 2 && p[1] == L':');
}

std::wstring join(const std::wstring& a, const std::wstring& b) {
    if (b.empty()) return a;
    if (a.empty() || rooted(b)) return b;
    wchar_t last = a.back();
    if (isSep(last) || last == L':') return a + b;
    return a + L"\\" + b;
}

std::wstring fileName(const std::wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    if (i == std::wstring::npos) return p.size() >= 2 && p[1] == L':' ? p.substr(2) : p;
    return p.substr(i + 1);
}

std::wstring pathRoot(const std::wstring& p) {
    if (p.size() >= 2 && p[1] == L':') return p.size() >= 3 && isSep(p[2]) ? p.substr(0, 3) : p.substr(0, 2);
    if (p.size() >= 2 && isSep(p[0]) && isSep(p[1])) {   // \\server\share
        size_t s = p.find_first_of(L"\\/", 2);
        if (s == std::wstring::npos) return p;
        size_t e = p.find_first_of(L"\\/", s + 1);
        return e == std::wstring::npos ? p : p.substr(0, e);
    }
    if (!p.empty() && isSep(p[0])) return p.substr(0, 1);
    return L"";
}

std::wstring parentDir(const std::wstring& p) {
    std::wstring root = pathRoot(p);
    size_t i = p.find_last_of(L"\\/");
    if (i == std::wstring::npos || p.size() <= root.size()) return L"";
    if (i < root.size()) return root;
    size_t e = i;
    while (e > root.size() && isSep(p[e - 1])) e--;
    return p.substr(0, e);
}

std::wstring fullPath(const std::wstring& p) {
    if (p.empty()) return p;
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetFullPathNameW(p.c_str(), (DWORD)(sizeof buf / sizeof buf[0]), buf, nullptr);
    if (n == 0) return p;
    if (n < sizeof buf / sizeof buf[0]) return std::wstring(buf, n);
    std::wstring big(n, L'\0');
    n = GetFullPathNameW(p.c_str(), n, &big[0], nullptr);
    big.resize(n);
    return big;
}

std::wstring trimEndSlash(std::wstring p) {
    while (!p.empty() && p.back() == L'\\') p.pop_back();
    return p;
}

static std::vector<std::wstring> segments(const std::wstring& p, size_t from) {
    std::vector<std::wstring> v;
    size_t i = from;
    while (i < p.size()) {
        size_t j = p.find_first_of(L"\\/", i);
        if (j == std::wstring::npos) j = p.size();
        if (j > i) v.push_back(p.substr(i, j - i));
        i = j + 1;
    }
    return v;
}

std::wstring relativePath(const std::wstring& base, const std::wstring& path) {
    std::wstring b = fullPath(base), t = fullPath(path);
    std::wstring rb = pathRoot(b), rt = pathRoot(t);
    if (!ieq(trimEndSlash(rb), trimEndSlash(rt))) return t;
    auto sb = segments(b, rb.size()), st = segments(t, rt.size());
    size_t k = 0;
    while (k < sb.size() && k < st.size() && ieq(sb[k], st[k])) k++;
    std::wstring r;
    for (size_t i = k; i < sb.size(); i++) r += r.empty() ? L".." : L"\\..";
    for (size_t i = k; i < st.size(); i++) { if (!r.empty()) r += L"\\"; r += st[i]; }
    if (!t.empty() && isSep(t.back()) && !r.empty()) r += L"\\";
    return r.empty() ? L"." : r;
}

bool isUnder(const std::wstring& path, const std::wstring& dir) {
    if (path.empty()) return false;
    return istarts(fullPath(path), trimEndSlash(fullPath(dir)) + L"\\");
}

bool fileExists(const std::wstring& p) {
    if (p.empty()) return false;
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool dirExists(const std::wstring& p) {
    if (p.empty()) return false;
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool createDirs(const std::wstring& p) {
    if (p.empty()) return false;
    if (dirExists(p)) return true;
    std::wstring parent = parentDir(trimEndSlash(p));
    if (!parent.empty() && parent != p && !dirExists(parent) && !createDirs(parent)) return false;
    if (CreateDirectoryW(p.c_str(), nullptr)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS && dirExists(p);
}

std::vector<std::wstring> subdirs(const std::wstring& d) {
    std::vector<std::wstring> v;
    if (d.empty()) return v;
    std::wstring pat = isSep(d.back()) ? d + L"*" : d + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(pat.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return v;
    std::wstring base = isSep(d.back()) ? d : d + L"\\";
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        v.push_back(base + fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return v;
}

int64_t fileSize(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a) || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return -1;
    return ((int64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
}

bool readFile(const std::wstring& p, std::string* out, DWORD* err) {
    Handle h(CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!h.ok()) { if (err) *err = GetLastError(); return false; }
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart > 0x7FFFFFFF) { if (err) *err = ERROR_FILE_TOO_LARGE; return false; }
    out->resize((size_t)sz.QuadPart);
    DWORD got = 0, total = 0;
    while (total < (DWORD)sz.QuadPart) {
        if (!ReadFile(h, &(*out)[total], (DWORD)sz.QuadPart - total, &got, nullptr)) { if (err) *err = GetLastError(); return false; }
        if (got == 0) break;
        total += got;
    }
    out->resize(total);
    return true;
}

static bool writeImpl(const std::wstring& p, const std::string& data, DWORD disp) {
    Handle h(CreateFileW(p.c_str(), disp == OPEN_ALWAYS ? FILE_APPEND_DATA : GENERIC_WRITE, FILE_SHARE_READ, nullptr, disp, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!h.ok()) return false;
    DWORD w = 0;
    return data.empty() || (WriteFile(h, data.data(), (DWORD)data.size(), &w, nullptr) && w == data.size());
}

bool writeFile(const std::wstring& p, const std::string& data) { return writeImpl(p, data, CREATE_ALWAYS); }
bool appendFile(const std::wstring& p, const std::string& data) { return writeImpl(p, data, OPEN_ALWAYS); }

std::wstring getEnv(const wchar_t* name) {
    wchar_t buf[1024];
    DWORD n = GetEnvironmentVariableW(name, buf, 1024);
    if (n == 0) return L"";
    if (n < 1024) return std::wstring(buf, n);
    std::wstring big(n, L'\0');
    n = GetEnvironmentVariableW(name, &big[0], n);
    big.resize(n);
    return big;
}

std::wstring expandEnv(const std::wstring& s) {
    if (s.find(L'%') == std::wstring::npos) return s;
    DWORD n = ExpandEnvironmentStringsW(s.c_str(), nullptr, 0);
    if (!n) return s;
    std::wstring o(n, L'\0');
    n = ExpandEnvironmentStringsW(s.c_str(), &o[0], n);
    o.resize(n ? n - 1 : 0);
    return o;
}

std::wstring selfExe() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof buf / sizeof buf[0]));
    return std::wstring(buf, n);
}

std::wstring knownFolder(const GUID& id) {
    PWSTR p = nullptr;
    std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) s = p;
    CoTaskMemFree(p);
    return s;
}

std::wstring winError(DWORD code) {
    wchar_t* buf = nullptr;
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_IGNORE_INSERTS;
    HMODULE mod = nullptr;
    if (code >= 12000 && code < 12200) { mod = GetModuleHandleW(L"winhttp.dll"); flags |= FORMAT_MESSAGE_FROM_HMODULE; }
    else flags |= FORMAT_MESSAGE_FROM_SYSTEM;
    DWORD n = FormatMessageW(flags, mod, code, 0, (LPWSTR)&buf, 0, nullptr);
    std::wstring s = n ? std::wstring(buf, n) : fmt(L"error %lu", code);
    LocalFree(buf);
    while (!s.empty() && (isWhite(s.back()))) s.pop_back();
    return s;
}

std::wstring ioError(DWORD code, const std::wstring& path) {
    switch (code) {
    case ERROR_FILE_NOT_FOUND: return L"Could not find file '" + path + L"'.";
    case ERROR_PATH_NOT_FOUND: return L"Could not find a part of the path '" + path + L"'.";
    case ERROR_ACCESS_DENIED: return L"Access to the path '" + path + L"' is denied.";
    case ERROR_SHARING_VIOLATION: return L"The process cannot access the file '" + path + L"' because it is being used by another process.";
    default: return winError(code) + L" : '" + path + L"'";
    }
}

// ------------------------------------------------------------------------------------------------ time
static void localNow(SYSTEMTIME* st, uint64_t* ticks, int* biasMin) {
    FILETIME utc, loc;
    GetSystemTimePreciseAsFileTime(&utc);
    FileTimeToLocalFileTime(&utc, &loc);
    ULARGE_INTEGER u, l;
    u.LowPart = utc.dwLowDateTime; u.HighPart = utc.dwHighDateTime;
    l.LowPart = loc.dwLowDateTime; l.HighPart = loc.dwHighDateTime;
    FileTimeToSystemTime(&loc, st);
    if (ticks) *ticks = l.QuadPart % 10000000ull;
    if (biasMin) *biasMin = (int)(((int64_t)l.QuadPart - (int64_t)u.QuadPart) / 600000000ll);
}

std::wstring nowIso() {
    SYSTEMTIME st; uint64_t t; int b;
    localNow(&st, &t, &b);
    int a = b < 0 ? -b : b;
    return fmt(L"%04d-%02d-%02dT%02d:%02d:%02d.%07llu%lc%02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
               (unsigned long long)t, b < 0 ? L'-' : L'+', a / 60, a % 60);
}

std::wstring nowTime(const wchar_t* p) {
    SYSTEMTIME st;
    localNow(&st, nullptr, nullptr);
    std::wstring o;
    for (size_t i = 0; p[i];) {
        auto run = [&](wchar_t c) { size_t n = 0; while (p[i + n] == c) n++; return n; };
        wchar_t c = p[i];
        size_t n = (c == L'y' || c == L'M' || c == L'd' || c == L'H' || c == L'm' || c == L's') ? run(c) : 0;
        if (!n) { o += c; i++; continue; }
        int v = c == L'y' ? st.wYear : c == L'M' ? st.wMonth : c == L'd' ? st.wDay : c == L'H' ? st.wHour : c == L'm' ? st.wMinute : st.wSecond;
        o += fmt(n >= 4 ? L"%04d" : L"%02d", v);
        i += n;
    }
    return o;
}

// ------------------------------------------------------------------------------------------------ console
static Lock g_outLock;

static void writeStd(DWORD which, const std::wstring& line) {
    HANDLE h = GetStdHandle(which);
    if (!h || h == INVALID_HANDLE_VALUE) return;
    Guard g(g_outLock);
    std::wstring l = line + L"\r\n";
    DWORD mode, w;
    if (GetConsoleMode(h, &mode)) WriteConsoleW(h, l.data(), (DWORD)l.size(), &w, nullptr);
    else { std::string u = narrow(l); WriteFile(h, u.data(), (DWORD)u.size(), &w, nullptr); }
}

void out(const std::wstring& line) { writeStd(STD_OUTPUT_HANDLE, line); }
void err(const std::wstring& line) { writeStd(STD_ERROR_HANDLE, line); }

// ------------------------------------------------------------------------------------------------ threads
static DWORD WINAPI asyncProc(LPVOID p) {
    auto* f = (std::function<void()>*)p;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    (*f)();
    if (SUCCEEDED(hr)) CoUninitialize();
    delete f;
    return 0;
}

void runAsync(std::function<void()> f) {
    auto* p = new std::function<void()>(std::move(f));
    HANDLE t = CreateThread(nullptr, 0, asyncProc, p, 0, nullptr);
    if (t) CloseHandle(t);
    else { asyncProc(p); }
}

} // namespace rn
