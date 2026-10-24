#include "proc.h"
#include <shellapi.h>
#include <tlhelp32.h>
#include <algorithm>

namespace rn {
namespace proc {

std::vector<Info> list() {
    std::vector<Info> v;
    Handle snap(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snap.ok()) return v;
    PROCESSENTRY32W e = { sizeof e };
    if (!Process32FirstW(snap, &e)) return v;
    do v.push_back(Info{ e.th32ProcessID, e.th32ParentProcessID, e.szExeFile });
    while (Process32NextW(snap, &e));
    return v;
}

std::wstring shortName(const std::wstring& exe) {
    std::wstring n = fileName(exe);
    return iends(n, L".exe") ? n.substr(0, n.size() - 4) : n;
}

std::wstring imagePath(DWORD pid) {
    Handle h(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!h.ok()) return L"";
    wchar_t buf[1024];
    DWORD n = 1024;
    if (QueryFullProcessImageNameW(h, 0, buf, &n)) return std::wstring(buf, n);
    std::wstring big(32768, L'\0');
    n = (DWORD)big.size();
    if (!QueryFullProcessImageNameW(h, 0, &big[0], &n)) return L"";
    big.resize(n);
    return big;
}

HANDLE newJob() {
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) return nullptr;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof info);
    return job;
}

HANDLE toolsJob() {
    static HANDLE job = newJob();
    return job;
}

namespace {

// The environment block with s.env applied: inherited variables, replaced case-insensitively, sorted like Windows
// expects (case-insensitive by key).
std::vector<wchar_t> envBlock(const std::vector<std::pair<std::wstring, std::wstring>>& changes) {
    std::vector<std::pair<std::wstring, std::wstring>> vars;
    wchar_t* base = GetEnvironmentStringsW();
    for (wchar_t* p = base; p && *p; p += wcslen(p) + 1) {
        std::wstring e = p;
        size_t eq = e.find(L'=', 1);   // "=C:=C:\x" keeps its leading '=' in the key
        if (eq == std::wstring::npos) continue;
        vars.emplace_back(e.substr(0, eq), e.substr(eq + 1));
    }
    if (base) FreeEnvironmentStringsW(base);
    for (auto& c : changes) {
        bool found = false;
        for (auto& v : vars) if (ieq(v.first, c.first)) { v.second = c.second; found = true; }
        if (!found) vars.push_back(c);
    }
    std::stable_sort(vars.begin(), vars.end(), [](const auto& a, const auto& b) {
        return CompareStringOrdinal(a.first.data(), (int)a.first.size(), b.first.data(), (int)b.first.size(), TRUE) == CSTR_LESS_THAN;
    });
    std::vector<wchar_t> block;
    for (auto& v : vars) {
        block.insert(block.end(), v.first.begin(), v.first.end());
        block.push_back(L'=');
        block.insert(block.end(), v.second.begin(), v.second.end());
        block.push_back(0);
    }
    block.push_back(0);
    if (block.size() == 1) block.push_back(0);
    return block;
}

HANDLE nulHandle(DWORD access) {
    SECURITY_ATTRIBUTES sa = { sizeof sa, nullptr, TRUE };
    return CreateFileW(L"NUL", access, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
}

} // namespace

HANDLE start(const Start& s, DWORD* pid, std::wstring* error) {
    std::wstring cmd = L"\"" + s.exe + L"\"";
    if (!s.args.empty()) cmd += L" " + s.args;
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(0);

    STARTUPINFOEXW si = {};
    si.StartupInfo.cb = sizeof si;
    DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | s.priority;
    if (s.hidden) { flags |= CREATE_NO_WINDOW; si.StartupInfo.dwFlags |= STARTF_USESHOWWINDOW; si.StartupInfo.wShowWindow = SW_HIDE; }

    Handle nulIn, nulOut;
    HANDLE inherit[3];
    int ni = 0;
    if (s.out || s.err) {
        nulIn.reset(nulHandle(GENERIC_READ));
        if (!s.out || !s.err) nulOut.reset(nulHandle(GENERIC_WRITE));
        si.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput = nulIn;
        si.StartupInfo.hStdOutput = s.out ? s.out : nulOut.h;
        si.StartupInfo.hStdError = s.err ? s.err : nulOut.h;
        inherit[ni++] = si.StartupInfo.hStdInput;
        inherit[ni++] = si.StartupInfo.hStdOutput;
        if (si.StartupInfo.hStdError != si.StartupInfo.hStdOutput) inherit[ni++] = si.StartupInfo.hStdError;
    }

    std::vector<wchar_t> env;
    if (!s.env.empty()) env = envBlock(s.env);

    HANDLE jobs[1] = { s.job };
    for (int attempt = 0; attempt < 2; attempt++) {
        bool withJob = s.job && attempt == 0;
        int attrs = (withJob ? 1 : 0) + (ni ? 1 : 0);
        std::vector<char> list;
        si.lpAttributeList = nullptr;
        if (attrs) {
            SIZE_T size = 0;
            InitializeProcThreadAttributeList(nullptr, attrs, 0, &size);
            list.resize(size);
            si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)list.data();
            InitializeProcThreadAttributeList(si.lpAttributeList, attrs, 0, &size);
            if (withJob) UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_JOB_LIST, jobs, sizeof jobs, nullptr, nullptr);
            if (ni) UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, ni * sizeof(HANDLE), nullptr, nullptr);
        }
        // Without the job attribute (older Windows), the process starts suspended and joins the job before it runs.
        DWORD f = flags | (s.job && !withJob ? CREATE_SUSPENDED : 0);
        PROCESS_INFORMATION pi = {};
        BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, ni ? TRUE : FALSE, f, env.empty() ? nullptr : env.data(),
                                 s.cwd.empty() ? nullptr : s.cwd.c_str(), &si.StartupInfo, &pi);
        DWORD code = ok ? 0 : GetLastError();
        if (si.lpAttributeList) DeleteProcThreadAttributeList(si.lpAttributeList);
        if (!ok) {
            if (withJob && code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND && code != ERROR_ACCESS_DENIED) continue;
            if (error) *error = winError(code);
            return nullptr;
        }
        if (s.job && !withJob) { AssignProcessToJobObject(s.job, pi.hProcess); ResumeThread(pi.hThread); }
        CloseHandle(pi.hThread);
        if (pid) *pid = pi.dwProcessId;
        return pi.hProcess;
    }
    return nullptr;
}

namespace {

// Splits a byte stream into lines like Process.OutputDataReceived: "\r", "\n" and "\r\n" end a line; a last line
// without an end is still delivered.
struct LineReader {
    HANDLE pipe;
    bool isErr;
    const std::function<void(const std::wstring&, bool)>* onLine;
    void operator()() {
        std::string line;
        bool lastCr = false;
        char buf[4096];
        DWORD got = 0;
        while (ReadFile(pipe, buf, sizeof buf, &got, nullptr) && got) {
            for (DWORD i = 0; i < got; i++) {
                char c = buf[i];
                if (c == '\n' && lastCr) { lastCr = false; continue; }
                lastCr = c == '\r';
                if (c == '\r' || c == '\n') { emit(line); line.clear(); continue; }
                line += c;
            }
        }
        if (!line.empty()) emit(line);
    }
    void emit(std::string& l) {
        if (l.size() >= 3 && (uint8_t)l[0] == 0xEF && (uint8_t)l[1] == 0xBB && (uint8_t)l[2] == 0xBF) l.erase(0, 3);
        (*onLine)(widen(l), isErr);
    }
};

DWORD WINAPI readerProc(LPVOID p) { (*(LineReader*)p)(); return 0; }

} // namespace

int run(Start s, const std::function<void(const std::wstring&, bool)>& onLine, std::wstring* error) {
    SECURITY_ATTRIBUTES sa = { sizeof sa, nullptr, TRUE };
    HANDLE outR = nullptr, outW = nullptr, errR = nullptr, errW = nullptr;
    if (!CreatePipe(&outR, &outW, &sa, 0) || !CreatePipe(&errR, &errW, &sa, 0)) {
        if (error) *error = winError(GetLastError());
        if (outR) { CloseHandle(outR); CloseHandle(outW); }
        return -1;
    }
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);
    s.out = outW;
    s.err = errW;
    DWORD pid = 0;
    Handle p(start(s, &pid, error));
    CloseHandle(outW);
    CloseHandle(errW);
    if (!p.ok()) { CloseHandle(outR); CloseHandle(errR); return -1; }
    if (s.started) s.started(pid);
    LineReader re{ errR, true, &onLine }, ro{ outR, false, &onLine };
    HANDLE t = CreateThread(nullptr, 0, readerProc, &re, 0, nullptr);
    ro();
    if (t) { WaitForSingleObject(t, INFINITE); CloseHandle(t); } else re();
    CloseHandle(outR);
    CloseHandle(errR);
    WaitForSingleObject(p, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(p, &code);
    return (int)code;
}

namespace {

bool creationTime(DWORD pid, ULONGLONG* t) {
    Handle h(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    FILETIME c, e, k, u;
    if (!h.ok() || !GetProcessTimes(h, &c, &e, &k, &u)) return false;
    *t = ((ULONGLONG)c.dwHighDateTime << 32) | c.dwLowDateTime;
    return true;
}

// Children of pid in the snapshot that started after it (a reused parent id is not a parent).
std::vector<DWORD> children(DWORD pid, const std::vector<Info>& snap) {
    std::vector<DWORD> v;
    ULONGLONG pt = 0;
    bool known = creationTime(pid, &pt);
    for (auto& i : snap) {
        if (i.ppid != pid || i.pid == pid || i.pid == 0) continue;
        ULONGLONG ct = 0;
        if (known && creationTime(i.pid, &ct) && ct < pt) continue;
        v.push_back(i.pid);
    }
    return v;
}

bool inTree(DWORD root, DWORD target, const std::vector<Info>& snap, int depth = 0) {
    if (root == target) return true;
    if (depth > 64) return false;
    for (DWORD c : children(root, snap)) if (inTree(c, target, snap, depth + 1)) return true;
    return false;
}

bool killOne(DWORD pid) {
    Handle h(OpenProcess(PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!h.ok()) return GetLastError() == ERROR_INVALID_PARAMETER;   // already gone
    if (TerminateProcess(h, (UINT)-1)) return true;
    DWORD code = 0;
    return GetExitCodeProcess(h, &code) && code != STILL_ACTIVE;
}

void killChildren(DWORD pid, const std::vector<Info>& snap, int depth) {
    if (depth > 64) return;
    for (DWORD c : children(pid, snap)) { killOne(c); killChildren(c, snap, depth + 1); }
}

} // namespace

bool killTree(DWORD pid, const std::vector<Info>& snap) {
    // Process.Kill(true) refuses a tree that contains the calling process
    if (inTree(pid, GetCurrentProcessId(), snap)) return false;
    auto kids = children(pid, snap);   // before the parent goes (its creation time is still readable)
    bool ok = killOne(pid);
    for (DWORD c : kids) { killOne(c); killChildren(c, snap, 1); }
    return ok;
}

namespace {

struct FindMain {
    DWORD pid;
    HWND best = nullptr, any = nullptr;
};

BOOL CALLBACK findMainProc(HWND h, LPARAM l) {
    auto* f = (FindMain*)l;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != f->pid) return TRUE;
    wchar_t cls[64];
    if (GetClassNameW(h, cls, 64) && !wcscmp(cls, L"RadarniteLauncher3") && !(GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)) { f->best = h; return FALSE; }
    if (!f->any && IsWindowVisible(h) && !GetWindow(h, GW_OWNER) && !(GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)) f->any = h;
    return TRUE;
}

} // namespace

HWND mainWindow() {
    FindMain f{ GetCurrentProcessId() };
    EnumWindows(findMainProc, (LPARAM)&f);
    return f.best ? f.best : f.any;
}

bool runElevated(const std::wstring& args) {
    std::wstring exe = selfExe();
    SHELLEXECUTEINFOW se = { sizeof se };
    se.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    se.hwnd = mainWindow();
    se.lpVerb = L"runas";
    se.lpFile = exe.c_str();
    se.lpParameters = args.c_str();
    se.nShow = SW_HIDE;
    if (!ShellExecuteExW(&se) || !se.hProcess) return false;   // UAC cancelled
    WaitForSingleObject(se.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(se.hProcess, &code);
    CloseHandle(se.hProcess);
    return code == 0;
}

bool open(const std::wstring& exe, const std::wstring& args) {
    SHELLEXECUTEINFOW se = { sizeof se };
    se.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    se.lpVerb = L"open";
    se.lpFile = exe.c_str();
    se.lpParameters = args.empty() ? nullptr : args.c_str();
    se.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&se) != FALSE;
}

} // namespace proc
} // namespace rn
