#include "net.h"
#include "util.h"
#include <bcrypt.h>
#include <winhttp.h>
#include <functional>

namespace rn {
namespace net {
namespace {

struct Hi {
    HINTERNET h = nullptr;
    Hi() = default;
    Hi(const Hi&) = delete;
    Hi& operator=(const Hi&) = delete;
    ~Hi() { if (h) WinHttpCloseHandle(h); }
};

struct Sha256 {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    bool ok = false;
    Sha256() {
        ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
             BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) >= 0;
    }
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    ~Sha256() {
        if (h) BCryptDestroyHash(h);
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    }
    void add(const void* p, size_t n) {
        while (ok && n) {
            ULONG k = n > 0x40000000 ? 0x40000000 : (ULONG)n;
            if (BCryptHashData(h, (PUCHAR)p, k, 0) < 0) ok = false;
            p = (const char*)p + k;
            n -= k;
        }
    }
    std::wstring done() {
        uint8_t d[32];
        if (!ok || BCryptFinishHash(h, d, sizeof d, 0) < 0) return L"";
        return hex(d, sizeof d);
    }
};

const wchar_t* reasonPhrase(int s) {
    switch (s) {
    case 301: return L"Moved Permanently";
    case 302: return L"Found";
    case 304: return L"Not Modified";
    case 400: return L"Bad Request";
    case 401: return L"Unauthorized";
    case 403: return L"Forbidden";
    case 404: return L"Not Found";
    case 408: return L"Request Timeout";
    case 429: return L"Too Many Requests";
    case 500: return L"Internal Server Error";
    case 502: return L"Bad Gateway";
    case 503: return L"Service Unavailable";
    case 504: return L"Gateway Timeout";
    default: return L"";
    }
}

std::wstring transportError(DWORD code) {
    switch (code) {
    case ERROR_WINHTTP_NAME_NOT_RESOLVED: return L"No such host is known.";
    case ERROR_WINHTTP_TIMEOUT: return L"The operation has timed out.";
    case ERROR_WINHTTP_CANNOT_CONNECT: return L"No connection could be made because the target machine actively refused it.";
    default: return winError(code);
    }
}

// GET url: status and headers checked, then the body handed to sink in chunks. false = error set.
bool fetch(const std::wstring& url, const wchar_t* ua, const wchar_t* headers, DWORD timeoutMs, int* status,
           std::wstring* error, const std::function<bool(const char*, DWORD)>& sink, bool decompress = false) {
    URL_COMPONENTS u = { sizeof u };
    u.dwSchemeLength = u.dwHostNameLength = u.dwUrlPathLength = u.dwExtraInfoLength = (DWORD)-1;
    if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &u)) { *error = L"Invalid URI: " + url; return false; }
    std::wstring host(u.lpszHostName, u.dwHostNameLength);
    std::wstring path = std::wstring(u.lpszUrlPath, u.dwUrlPathLength) + std::wstring(u.lpszExtraInfo, u.dwExtraInfoLength);
    if (path.empty()) path = L"/";
    bool loopback = host == L"127.0.0.1" || ieq(host, L"localhost");
    Hi s, c, r;
    if (loopback) s.h = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    else {
        s.h = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!s.h) s.h = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!s.h) { *error = transportError(GetLastError()); return false; }
    WinHttpSetTimeouts(s.h, (int)timeoutMs, (int)timeoutMs, (int)timeoutMs, (int)timeoutMs);
    c.h = WinHttpConnect(s.h, host.c_str(), u.nPort, 0);
    if (!c.h) { *error = transportError(GetLastError()); return false; }
    r.h = WinHttpOpenRequest(c.h, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                             u.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (!r.h) { *error = transportError(GetLastError()); return false; }
    if (decompress) {
        DWORD d = WINHTTP_DECOMPRESSION_FLAG_ALL;   // Windows 8.1+: Accept-Encoding gzip, deflate and inflated reads
        WinHttpSetOption(r.h, WINHTTP_OPTION_DECOMPRESSION, &d, sizeof d);
    }
    if (headers && *headers) WinHttpAddRequestHeaders(r.h, headers, (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    if (!WinHttpSendRequest(r.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(r.h, nullptr)) {
        *error = transportError(GetLastError());
        return false;
    }
    DWORD st = 0, sz = sizeof st;
    WinHttpQueryHeaders(r.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &st, &sz, WINHTTP_NO_HEADER_INDEX);
    *status = (int)st;
    if (st < 200 || st > 299) {
        wchar_t text[128] = L"";
        DWORD tl = sizeof text;
        std::wstring reason;
        if (WinHttpQueryHeaders(r.h, WINHTTP_QUERY_STATUS_TEXT, WINHTTP_HEADER_NAME_BY_INDEX, text, &tl, WINHTTP_NO_HEADER_INDEX)) reason = trim(text);
        if (reason.empty()) reason = reasonPhrase((int)st);
        *error = fmt(L"Response status code does not indicate success: %lu (%ls).", st, reason.c_str());
        return false;
    }
    std::string buf(65536, '\0');
    for (;;) {
        DWORD got = 0;
        if (!WinHttpReadData(r.h, &buf[0], (DWORD)buf.size(), &got)) { *error = transportError(GetLastError()); return false; }
        if (got == 0) return true;
        if (!sink(buf.data(), got)) { *error = L"could not write the download"; return false; }
    }
}

} // namespace

Response get(const std::wstring& url, const wchar_t* ua, const wchar_t* headers, DWORD timeoutMs) {
    Response res;
    fetch(url, ua, headers, timeoutMs, &res.status, &res.error, [&](const char* p, DWORD n) { res.body.append(p, n); return true; });
    return res;
}

bool download(const std::wstring& url, const wchar_t* ua, DWORD timeoutMs, const std::wstring& file, std::wstring* sha, std::wstring* error) {
    Handle f(CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!f.ok()) { *error = ioError(GetLastError(), file); return false; }
    Sha256 h;
    int status = 0;
    bool ok = fetch(url, ua, nullptr, timeoutMs, &status, error, [&](const char* p, DWORD n) {
        DWORD w = 0;
        h.add(p, n);
        return WriteFile(f, p, n, &w, nullptr) && w == n;
    });
    if (ok && sha) *sha = h.done();
    return ok;
}

bool fetchFile(const std::wstring& url, const wchar_t* ua, DWORD timeoutMs, const std::wstring& file, std::wstring* error) {
    std::wstring part = file + L".part";
    bool ok;
    {
        Handle f(CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!f.ok()) { *error = ioError(GetLastError(), part); return false; }
        int status = 0;
        ok = fetch(url, ua, nullptr, timeoutMs, &status, error, [&](const char* p, DWORD n) {
            DWORD w = 0;
            return WriteFile(f, p, n, &w, nullptr) && w == n;
        }, true);
    }
    if (ok && !MoveFileExW(part.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING)) { *error = ioError(GetLastError(), file); ok = false; }
    if (!ok) DeleteFileW(part.c_str());
    return ok;
}

std::wstring sha256(const void* data, size_t n) {
    Sha256 h;
    h.add(data, n);
    return h.done();
}

std::wstring escapeData(const std::wstring& s) {
    std::string u = narrow(s);
    std::wstring o;
    for (unsigned char c : u) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~') o += (wchar_t)c;
        else o += fmt(L"%%%02X", c);
    }
    return o;
}

} // namespace net
} // namespace rn
