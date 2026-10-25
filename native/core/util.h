// Radarnite Launcher 3.0 core: small helpers that mirror the .NET calls launcher/*.cs relies on (Path.*, File.*,
// Directory.*, DateTime formats, Console.WriteLine), so the ported code reads like the C# it comes from.
#pragma once
#include <windows.h>
#include <stdint.h>
#include <functional>
#include <string>
#include <vector>

namespace rn {

using LogFn = std::function<void(const std::wstring&)>;

// ---- strings
std::wstring widen(const std::string& utf8);           // invalid bytes become U+FFFD (like File.ReadAllText)
std::string narrow(const std::wstring& w);
std::wstring upper(const std::wstring& s);             // key for OrdinalIgnoreCase sets
bool ieq(const std::wstring& a, const std::wstring& b);   // StringComparison.OrdinalIgnoreCase
bool istarts(const std::wstring& s, const std::wstring& prefix);
bool iends(const std::wstring& s, const std::wstring& suffix);
bool starts(const std::wstring& s, const wchar_t* prefix);
bool contains(const std::wstring& s, const std::wstring& part);
bool isWhite(wchar_t c);                               // char.IsWhiteSpace
std::wstring trim(const std::wstring& s);              // string.Trim()
bool blank(const std::wstring& s);                     // string.IsNullOrWhiteSpace
std::wstring fmt(const wchar_t* f, ...);
std::wstring hex(const uint8_t* p, size_t n);          // lower-case

// ---- paths and files (.NET semantics on Windows)
std::wstring join(const std::wstring& a, const std::wstring& b);   // Path.Combine
std::wstring fileName(const std::wstring& p);          // Path.GetFileName
std::wstring parentDir(const std::wstring& p);         // Path.GetDirectoryName ("" for a root)
std::wstring fullPath(const std::wstring& p);          // Path.GetFullPath
std::wstring pathRoot(const std::wstring& p);          // Path.GetPathRoot ("S:\\")
std::wstring trimEndSlash(std::wstring p);             // TrimEnd('\\')
std::wstring relativePath(const std::wstring& base, const std::wstring& path);   // Path.GetRelativePath
bool isUnder(const std::wstring& path, const std::wstring& dir);   // Paths.IsUnder
bool fileExists(const std::wstring& p);
bool dirExists(const std::wstring& p);
bool createDirs(const std::wstring& p);                // Directory.CreateDirectory (all levels)
std::vector<std::wstring> subdirs(const std::wstring& d);   // Directory.GetDirectories, [] on any error
int64_t fileSize(const std::wstring& p);               // -1 when it does not exist
bool readFile(const std::wstring& p, std::string* out, DWORD* err = nullptr);
bool writeFile(const std::wstring& p, const std::string& data);
bool appendFile(const std::wstring& p, const std::string& data);
std::wstring getEnv(const wchar_t* name);
std::wstring expandEnv(const std::wstring& s);         // Environment.ExpandEnvironmentVariables
std::wstring selfExe();
std::wstring knownFolder(const GUID& id);              // SHGetKnownFolderPath
std::wstring winError(DWORD code);                     // FormatMessage text without the trailing period/newline
std::wstring ioError(DWORD code, const std::wstring& path);   // the .NET IOException text for common codes

// ---- time (local clock)
std::wstring nowIso();                                 // DateTime.Now.ToString("O")
std::wstring isoLocal(const FILETIME& utc);            // a file time as local DateTime.ToString("O")
std::wstring nowTime(const wchar_t* pattern);          // "HH:mm:ss", "yyyyMMddHHmmss" ... (y M d H m s only)

// ---- console: Console.WriteLine (UTF-16 to a console; to a pipe/file in the console output code page, the ANSI one
//      without a console, like .NET; nothing when there is no stdout)
void out(const std::wstring& line);
void err(const std::wstring& line);

// ---- threads: a detached worker running f (COM initialised for it)
void runAsync(std::function<void()> f);
// Runs every f at the same time (the first on this thread) and returns when all are done.
void parallel(std::initializer_list<std::function<void()>> fs);

// ---- RAII for handles
struct Handle {
    HANDLE h = nullptr;
    Handle() = default;
    explicit Handle(HANDLE x) : h(x) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { reset(); }
    void reset(HANDLE x = nullptr) { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); h = x; }
    bool ok() const { return h && h != INVALID_HANDLE_VALUE; }
    operator HANDLE() const { return h; }
};

struct Lock {
    SRWLOCK l = SRWLOCK_INIT;
    void lock() { AcquireSRWLockExclusive(&l); }
    void unlock() { ReleaseSRWLockExclusive(&l); }
};
struct Guard {
    Lock& l;
    explicit Guard(Lock& x) : l(x) { l.lock(); }
    ~Guard() { l.unlock(); }
};

} // namespace rn
