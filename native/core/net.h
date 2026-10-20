// HTTP(S) GET over WinHTTP (the system proxy settings, redirects followed) and SHA-256 over BCrypt: what the 2.x
// HttpClient and SHA256.HashData calls did.
#pragma once
#include <windows.h>
#include <stdint.h>
#include <string>

namespace rn {
namespace net {

struct Response {
    int status = 0;            // 0 = no response (error says why)
    std::string body;          // raw bytes
    std::wstring error;        // transport error, or HttpClient's EnsureSuccessStatusCode text for a non-2xx status
    bool ok() const { return error.empty() && status >= 200 && status <= 299; }
};

// GET url. headers: extra request headers ("Accept: ...\r\n"), or nullptr. timeoutMs applies to each step.
Response get(const std::wstring& url, const wchar_t* userAgent, const wchar_t* headers, DWORD timeoutMs);
// GET url into file (streamed), hashing it on the way. sha256Hex = lower-case digest of the bytes written.
bool download(const std::wstring& url, const wchar_t* userAgent, DWORD timeoutMs, const std::wstring& file,
              std::wstring* sha256Hex, std::wstring* error);

// Lower-case hex SHA-256 of a buffer ("" when BCrypt fails).
std::wstring sha256(const void* data, size_t n);

// Uri.EscapeDataString: everything but A-Z a-z 0-9 - . _ ~ percent-encoded (UTF-8, upper-case hex).
std::wstring escapeData(const std::wstring& s);

} // namespace net
} // namespace rn
