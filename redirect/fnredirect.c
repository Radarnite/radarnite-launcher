/*
 * fnredirect.dll - keeps an old Fortnite build talking ONLY to the local offline backend.
 *
 * Injected by the offline launcher into FortniteClient-Win64-Shipping.exe (only the build under S:\FortniteBuilds,
 * never the real install). It finds libcurl's curl_easy_setopt inside the game executable (statically linked by
 * Unreal's HTTP module) and hooks it:
 *   - CURLOPT_URL: every URL whose host is not 127.0.0.1/localhost is rewritten to http://127.0.0.1:<port><path>.
 *     So every HTTP request of the game (accounts, MCP, lightswitch, content, cloudstorage, datarouter, ...) goes to
 *     the local backend; nothing is sent to any other host. (The Windows Firewall rules block the rest anyway.)
 *   - CURLOPT_SSL_VERIFYPEER / CURLOPT_SSL_VERIFYHOST: forced to 0 (the backend is plain HTTP on loopback).
 * Settings come from the environment the launcher gives the game process:
 *   FNOFFLINE_PORT (default 3551), FNOFFLINE_LOG (log file path; optional),
 *   FNOFFLINE_SANDBOX (optional): per-user folders (LocalAppData, RoamingAppData, Documents, Saved Games) asked through
 *   SHGetKnownFolderPath (or SHGetFolderPathW: UE 4.20 builds such as 3.x) are answered with <sandbox>\<name>, so the build never writes in the real
 *   %LOCALAPPDATA%\FortniteGame (shared with the real Fortnite). Done by patching the import tables of the loaded
 *   modules; the launcher loads this DLL before the game's own code runs (early APC), so the engine sees the sandbox.
 *   FNOFFLINE_CURL_HOOK=hwbp: hook curl_easy_setopt with a hardware breakpoint instead of a jmp (3.x-15.x check their code).
 *   Diagnostics: FNOFFLINE_TRACE_CRASH=1 (access violations + DLL loads/unloads), FNOFFLINE_TRACE_EXIT=1,
 *   FNOFFLINE_BLOCK_SHELL=1 (open nothing through the shell), FNOFFLINE_NO_CURL_HOOK=1 (no redirect at all).
 * The game's requests to open com.epicgames.launcher:// (a silent "verify" at every start of 3.x-15.x) are never opened.
 * If curl_easy_setopt cannot be found unambiguously, nothing is patched and the log says so: the launcher then
 * reports the build as unsupported (the firewall still blocks every connection).
 *
 * Build (mingw-w64): x86_64-w64-mingw32-gcc -O2 -shared -s -o fnredirect.dll fnredirect.c
 */
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define CURLOPT_URL 10002
#define CURLOPT_SSL_VERIFYPEER 64
#define CURLOPT_SSL_VERIFYHOST 81

typedef int (*setopt_fn)(void *, int, void *);
static setopt_fn g_original;
static char g_prefix[64] = "http://127.0.0.1:3551";
static char g_log[MAX_PATH];
static volatile LONG g_logged;
static CRITICAL_SECTION g_log_lock;

static void logf_(const char *fmt, ...)
{
    if (!g_log[0]) return;
    EnterCriticalSection(&g_log_lock);
    FILE *f = fopen(g_log, "a");
    if (f) {
        SYSTEMTIME t; GetLocalTime(&t);
        fprintf(f, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
        va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
        fputc('\n', f);
        fclose(f);
    }
    LeaveCriticalSection(&g_log_lock);
}

static int is_local_host(const char *host, size_t len)
{
    return (len == 9 && !_strnicmp(host, "127.0.0.1", 9)) || (len == 9 && !_strnicmp(host, "localhost", 9));
}

/* Returns a malloc'ed rewritten URL, or NULL when the URL is already local (or not http/https). */
static char *rewrite(const char *url)
{
    const char *p = strstr(url, "://");
    if (!p) return NULL;
    const char *host = p + 3;
    const char *path = host + strcspn(host, "/?#");
    const char *hostend = host;
    while (hostend < path && *hostend != ':') hostend++;
    if (is_local_host(host, (size_t)(hostend - host))) return NULL;
    size_t n = strlen(g_prefix) + strlen(path) + 2;
    char *out = (char *)malloc(n);
    if (!out) return NULL;
    snprintf(out, n, "%s%s", g_prefix, *path == '/' ? path : "/");
    if (*path != '/' && *path) strncat(out, path, n - strlen(out) - 1);
    return out;
}

static int hook_setopt(void *handle, int option, void *arg)
{
    if (option == CURLOPT_URL && arg) {
        char *local = rewrite((const char *)arg);
        if (local) {
            if (InterlockedIncrement(&g_logged) <= 400) logf_("redirect %s -> %s", (const char *)arg, local);
            int r = g_original(handle, option, local); /* libcurl copies the string */
            free(local);
            return r;
        }
    } else if (option == CURLOPT_SSL_VERIFYPEER || option == CURLOPT_SSL_VERIFYHOST) {
        arg = (void *)0;
    }
    return g_original(handle, option, arg);
}

/*
 * curl_easy_setopt(CURL *data, CURLoption tag, ...) as MSVC compiles it in Unreal's libcurl:
 *   89 54 24 10        mov [rsp+10h], edx
 *   4C 89 44 24 18     mov [rsp+18h], r8
 *   4C 89 4C 24 20     mov [rsp+20h], r9        <- these first 14 bytes are position independent
 *   48 83 EC xx        sub rsp, xx
 *   48 85 C9           test rcx, rcx
 *   ...                 return CURLE_BAD_FUNCTION_ARGUMENT (43 = 0x2B) when data is NULL
 * The scan accepts the null check within 12 bytes and the 43 (either "8D 41 2B" or "B8 2B 00 00 00") within 24.
 */
static const unsigned char SPILL[14] = {0x89,0x54,0x24,0x10, 0x4C,0x89,0x44,0x24,0x18, 0x4C,0x89,0x4C,0x24,0x20};

static unsigned char *find_setopt(unsigned char *base, size_t size, int *count)
{
    unsigned char *found = NULL;
    *count = 0;
    for (size_t i = 0; i + 64 < size; i++) {
        unsigned char *p = base + i;
        if (p[0] != 0x89 || memcmp(p, SPILL, 14) != 0) continue;
        if (!(p[14] == 0x48 && p[15] == 0x83 && p[16] == 0xEC)) continue;
        int nullcheck = 0, ret43 = 0;
        for (int k = 18; k < 18 + 12; k++) if (p[k] == 0x48 && p[k+1] == 0x85 && p[k+2] == 0xC9) { nullcheck = 1; break; }
        if (!nullcheck) continue;
        for (int k = 18; k < 18 + 24; k++) {
            if (p[k] == 0x8D && p[k+1] == 0x41 && p[k+2] == 0x2B) { ret43 = 1; break; }
            if (p[k] == 0xB8 && p[k+1] == 0x2B && p[k+2] == 0 && p[k+3] == 0 && p[k+4] == 0) { ret43 = 1; break; }
        }
        if (!ret43) continue;
        (*count)++;
        if (!found) found = p;
    }
    return found;
}

/*
 * Hardware-breakpoint hook (FNOFFLINE_CURL_HOOK=hwbp): not one byte of the game's code is changed. Builds 3.x-15.x
 * check their own code at run time: the 14-byte jmp made 14.30 ask the Epic launcher to "verify" and then jump to a
 * garbage address (0xC0000005) right after the engine started, with no crash report. Here debug register DR0 of every
 * thread points at curl_easy_setopt; the vectored handler below rewrites its arguments (URL -> local backend, SSL
 * verification -> 0) and resumes it with the resume flag.
 */
static unsigned char *volatile g_bp_addr;
static int g_hwbp;
static char g_urlbuf[64][4096];
static volatile LONG g_urlnext;

static LONG WINAPI bp_handler(EXCEPTION_POINTERS *e)
{
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !g_bp_addr ||
        e->ExceptionRecord->ExceptionAddress != (void *)g_bp_addr) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT *c = e->ContextRecord;
    int option = (int)c->Rdx;
    if (option == CURLOPT_URL && c->R8) {
        char *local = rewrite((const char *)c->R8);
        if (local) {
            /* libcurl copies the string during the call: a ring of 64 buffers outlives it */
            char *buf = g_urlbuf[(ULONG)InterlockedIncrement(&g_urlnext) % 64];
            lstrcpynA(buf, local, sizeof g_urlbuf[0]);
            if (InterlockedIncrement(&g_logged) <= 400) logf_("redirect %s -> %s", (const char *)c->R8, buf);
            free(local);
            c->R8 = (DWORD64)buf;
        }
    } else if (option == CURLOPT_SSL_VERIFYPEER || option == CURLOPT_SSL_VERIFYHOST) {
        c->R8 = 0;
    }
    c->EFlags |= 0x10000; /* RF: run the instruction without hitting the breakpoint again */
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void set_dr(CONTEXT *c)
{
    c->Dr0 = (DWORD64)g_bp_addr;
    c->Dr7 = (c->Dr7 & ~(DWORD64)0x000F0003) | 1; /* L0, execute, length 1 */
}

/* The calling thread arms itself: RtlCaptureContext + NtContinue with the debug registers set. */
static void arm_self(void)
{
    typedef LONG (NTAPI *cont_fn)(CONTEXT *, BOOLEAN);
    static cont_fn cont;
    if (!g_bp_addr) return;
    if (!cont) cont = (cont_fn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtContinue");
    if (!cont) return;
    volatile int done = 0;
    CONTEXT c; RtlCaptureContext(&c);
    if (!done) { done = 1; set_dr(&c); c.ContextFlags = CONTEXT_FULL | CONTEXT_DEBUG_REGISTERS; cont(&c, FALSE); }
}

/* Every other thread of the process (they are suspended for the few microseconds it takes).
 * The thread list comes from NtQuerySystemInformation, not Toolhelp: Thread32First/Next map the snapshot with
 * MapViewOfFile, and once the game loads CEF for the login screen chrome_elf hooks NtMapViewOfSection with a
 * thunk that jumps to null from foreign threads (3.6: client and match server died at "Login", 0xC0000005,
 * stack fnredirect -> Thread32Next -> chrome_elf -> 0). */
typedef struct { LARGE_INTEGER kernel, user, create; ULONG wait; PVOID start; HANDLE pid, tid; LONG prio, base; ULONG switches, state, reason; } sys_thread;
#define SYS_PROC_PID 0x50     /* SYSTEM_PROCESS_INFORMATION.UniqueProcessId (x64) */
#define SYS_PROC_THREADS 0x100 /* its SYSTEM_THREAD_INFORMATION array follows the 0x100-byte header (x64) */
static int arm_others(void)
{
    typedef LONG (NTAPI *qsi_fn)(ULONG, PVOID, ULONG, PULONG);
    static qsi_fn qsi;
    static unsigned char *buf;
    static ULONG cap;
    if (!qsi) qsi = (qsi_fn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQuerySystemInformation");
    if (!qsi) return 0;
    ULONG need = 0;
    LONG st;
    for (int tries = 0; tries < 4; tries++) {
        if (!buf || need > cap) {
            if (buf) VirtualFree(buf, 0, MEM_RELEASE);
            cap = need > cap ? need + (need >> 1) : (cap ? cap : 1u << 20);
            buf = VirtualAlloc(NULL, cap, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!buf) { cap = 0; return 0; }
        }
        st = qsi(5 /* SystemProcessInformation */, buf, cap, &need);
        if (st != (LONG)0xC0000004 /* STATUS_INFO_LENGTH_MISMATCH */) break;
        if (need <= cap) need = cap * 2;
    }
    if (st < 0) return 0;
    int n = 0;
    DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
    for (unsigned char *p = buf;; ) {
        ULONG next = *(ULONG *)p, count = *(ULONG *)(p + 4);
        if ((DWORD)(ULONG_PTR)*(HANDLE *)(p + SYS_PROC_PID) == pid) {
            sys_thread *th = (sys_thread *)(p + SYS_PROC_THREADS);
            for (ULONG i = 0; i < count; i++) {
                DWORD tid = (DWORD)(ULONG_PTR)th[i].tid;
                if (tid == me) continue;
                HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, tid);
                if (!t) continue;
                if (SuspendThread(t) != (DWORD)-1) {
                    CONTEXT c = { 0 }; c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                    if (GetThreadContext(t, &c) && (c.Dr0 != (DWORD64)g_bp_addr || !(c.Dr7 & 1))) {
                        set_dr(&c);
                        if (SetThreadContext(t, &c)) n++;
                    }
                    ResumeThread(t);
                }
                CloseHandle(t);
            }
            break;
        }
        if (!next) break;
        p += next;
    }
    return n;
}

/* Safety net: threads that started before the address was known, or whose registers were cleared. */
static DWORD WINAPI rearm_thread(LPVOID p)
{
    (void)p;
    for (int i = 0;; i++) {
        int n = arm_others();
        static int told;
        if (n && i > 0 && told++ < 5) logf_("hwbp: armed %d more threads%s", n, told == 5 ? " (not logged any more)" : "");
        Sleep(i < 600 ? 100 : 1000);
    }
    return 0;
}

static void hwbp_start(unsigned char *target)
{
    g_bp_addr = target;
    AddVectoredExceptionHandler(1, bp_handler);
    arm_self();
    int n = arm_others();
    logf_("hwbp: curl_easy_setopt breakpoint on this thread and %d others", n);
    CloseHandle(CreateThread(NULL, 0, rearm_thread, NULL, 0, NULL));
}

static int install_quiet(int quiet)
{
    HMODULE exe = GetModuleHandleW(NULL);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)exe;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)((unsigned char *)exe + dos->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    unsigned char *target = NULL;
    int total = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        /* scan only committed, readable pages (some sections may be reserved or no-access this early) */
        unsigned char *p = (unsigned char *)exe + sec->VirtualAddress, *end = p + sec->Misc.VirtualSize;
        while (p < end) {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery(p, &mbi, sizeof mbi)) break;
            unsigned char *rend = (unsigned char *)mbi.BaseAddress + mbi.RegionSize;
            if (rend > end) rend = end;
            DWORD prot = mbi.Protect & 0xFF;
            int readable = mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) &&
                (prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY || prot == PAGE_READONLY || prot == PAGE_READWRITE);
            if (readable && rend - p > 64) {
                int c = 0;
                unsigned char *t = find_setopt(p, (size_t)(rend - p), &c);
                if (t && !target) target = t;
                total += c;
            } else if (!readable && !quiet) logf_("skip region %p+%llx state=%lx prot=%lx", p, (unsigned long long)(rend - p), mbi.State, mbi.Protect);
            p = rend;
        }
    }
    if (!target || total != 1) {
        if (!quiet) logf_("UNSUPPORTED: curl_easy_setopt matches = %d (need exactly 1); nothing patched", total);
        return total > 1 ? -1 : 0;
    }
    if (g_hwbp) {
        hwbp_start(target);
        logf_("OK: curl_easy_setopt hooked at exe+0x%llx (hardware breakpoint, code untouched), redirecting to %s",
              (unsigned long long)(target - (unsigned char *)exe), g_prefix);
        return 1;
    }
    unsigned char *tramp = (unsigned char *)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) { logf_("VirtualAlloc failed"); return 0; }
    memcpy(tramp, target, 14);
    unsigned char jmpback[14] = {0xFF,0x25,0,0,0,0};
    *(unsigned long long *)(jmpback + 6) = (unsigned long long)(target + 14);
    memcpy(tramp + 14, jmpback, 14);
    FlushInstructionCache(GetCurrentProcess(), tramp, 28);
    g_original = (setopt_fn)tramp;

    unsigned char jmp[14] = {0xFF,0x25,0,0,0,0};
    *(unsigned long long *)(jmp + 6) = (unsigned long long)&hook_setopt;
    DWORD old;
    if (!VirtualProtect(target, 14, PAGE_EXECUTE_READWRITE, &old)) { logf_("VirtualProtect failed"); return 0; }
    memcpy(target, jmp, 14);
    VirtualProtect(target, 14, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, 14);
    logf_("OK: curl_easy_setopt hooked at exe+0x%llx, redirecting to %s",
          (unsigned long long)(target - (unsigned char *)exe), g_prefix);
    return 1;
}

/* ---------------- known-folder sandbox ---------------- */
static wchar_t g_sandbox[MAX_PATH];
typedef HRESULT (WINAPI *known_fn)(const GUID *, DWORD, HANDLE, PWSTR *);
static known_fn g_known_orig;
static const GUID G_LocalAppData   = {0xF1B32785,0x6FBA,0x4FCF,{0x9D,0x55,0x7B,0x8E,0x7F,0x15,0x70,0x91}};
static const GUID G_RoamingAppData = {0x3EB685DB,0x65F9,0x4CF6,{0xA0,0x3A,0xE3,0xEF,0x65,0x72,0x9F,0x3D}};
static const GUID G_Documents      = {0xFDD39AD0,0x238F,0x46AF,{0xAD,0xB4,0x6C,0x85,0x48,0x03,0x69,0xC7}};
static const GUID G_SavedGames     = {0x4C5C32FF,0xBB9D,0x43B0,{0xB5,0xB4,0x2D,0x72,0xE5,0x4E,0xAA,0xA4}};
static const GUID G_LocalAppDataLow= {0xA520A1A4,0x1780,0x4FF6,{0xBD,0x18,0x16,0x73,0x43,0xC5,0xAF,0x16}};

static HRESULT WINAPI hook_known(const GUID *id, DWORD flags, HANDLE token, PWSTR *out)
{
    const wchar_t *sub = NULL;
    if (IsEqualGUID(id, &G_LocalAppData)) sub = L"LocalAppData";
    else if (IsEqualGUID(id, &G_RoamingAppData)) sub = L"RoamingAppData";
    else if (IsEqualGUID(id, &G_Documents)) sub = L"Documents";
    else if (IsEqualGUID(id, &G_SavedGames)) sub = L"SavedGames";
    else if (IsEqualGUID(id, &G_LocalAppDataLow)) sub = L"LocalAppDataLow";
    if (!sub || !g_sandbox[0]) return g_known_orig(id, flags, token, out);
    wchar_t path[MAX_PATH];
    _snwprintf(path, MAX_PATH, L"%ls\\%ls", g_sandbox, sub);
    path[MAX_PATH - 1] = 0;
    CreateDirectoryW(g_sandbox, NULL);
    CreateDirectoryW(path, NULL);
    size_t n = (wcslen(path) + 1) * sizeof(wchar_t);
    /* CoTaskMemAlloc resolved at call time: no ole32 import (it must not load while the process is starting) */
    typedef LPVOID (WINAPI *ctma_fn)(SIZE_T);
    static ctma_fn ctma;
    if (!ctma) ctma = (ctma_fn)GetProcAddress(LoadLibraryA("ole32.dll"), "CoTaskMemAlloc");
    PWSTR mem = ctma ? (PWSTR)ctma(n) : NULL;
    if (!mem) return E_OUTOFMEMORY;
    memcpy(mem, path, n);
    *out = mem;
    return S_OK;
}

/* SHGetFolderPathW (CSIDL): what UE 4.20 (3.x) asks instead of SHGetKnownFolderPath. Same sandbox folders. */
typedef HRESULT (WINAPI *folder_fn)(HWND, int, HANDLE, DWORD, LPWSTR);
static folder_fn g_folder_orig;
static HRESULT WINAPI hook_folder(HWND hwnd, int csidl, HANDLE token, DWORD flags, LPWSTR out)
{
    const wchar_t *sub = NULL;
    switch (csidl & 0xFF) { /* without CSIDL_FLAG_CREATE / DONT_VERIFY */
    case 0x1C: sub = L"LocalAppData"; break;   /* CSIDL_LOCAL_APPDATA */
    case 0x1A: sub = L"RoamingAppData"; break; /* CSIDL_APPDATA */
    case 0x05: sub = L"Documents"; break;      /* CSIDL_PERSONAL */
    }
    if (!sub || !g_sandbox[0] || !out) return g_folder_orig ? g_folder_orig(hwnd, csidl, token, flags, out) : E_FAIL;
    _snwprintf(out, MAX_PATH, L"%ls\\%ls", g_sandbox, sub);
    out[MAX_PATH - 1] = 0;
    CreateDirectoryW(g_sandbox, NULL);
    CreateDirectoryW(out, NULL);
    return S_OK;
}

/* ---- diagnostics (FNOFFLINE_TRACE_EXIT=1): who asks the game to quit ---- */
typedef VOID (WINAPI *pqm_fn)(int);
static pqm_fn g_pqm_orig;
static int g_trace_exit;
static void log_stack(const char *what)
{
    void *frames[48]; USHORT n = RtlCaptureStackBackTrace(1, 48, frames, NULL);
    unsigned char *exe = (unsigned char *)GetModuleHandleW(NULL);
    logf_("%s: %u frames (exe base %p)", what, n, exe);
    for (USHORT i = 0; i < n; i++) {
        HMODULE m = NULL; char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)frames[i], &m) && m)
            GetModuleFileNameA(m, name, MAX_PATH);
        const char *base = strrchr(name, '\\'); base = base ? base + 1 : name;
        logf_("  #%u %s+0x%llx", i, base, (unsigned long long)((unsigned char *)frames[i] - (unsigned char *)m));
    }
}
static VOID WINAPI hook_pqm(int code) { log_stack("PostQuitMessage"); g_pqm_orig(code); }

/* FNOFFLINE_TRACE_RVA=<hex>,<len>: log the call stack every time the function at exe+rva runs (diagnostics).
   <len> = bytes of whole, position-independent instructions at its start (>= 14), copied to a trampoline. */
typedef void *(*any4_fn)(void *, void *, void *, void *);
static any4_fn g_trace_orig;
void *g_regs[8];
void *hook_trace(void *a, void *b, void *c, void *d);
__asm__(".globl trace_stub\ntrace_stub:\n"
        " mov %rbx, g_regs(%rip)\n mov %rsi, g_regs+8(%rip)\n mov %rdi, g_regs+16(%rip)\n mov %rbp, g_regs+24(%rip)\n"
        " mov %r12, g_regs+32(%rip)\n mov %r13, g_regs+40(%rip)\n mov %r14, g_regs+48(%rip)\n mov %r15, g_regs+56(%rip)\n"
        " jmp hook_trace\n");
void trace_stub(void);
static int readable(const void *p, size_t n)
{
    MEMORY_BASIC_INFORMATION mi;
    if (!p || !VirtualQuery(p, &mi, sizeof mi) || mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
    return (const char *)p + n <= (const char *)mi.BaseAddress + mi.RegionSize;
}
static void log_ptr(const char *name, void *v)
{
    unsigned char *exe = (unsigned char *)GetModuleHandleW(NULL);
    char line[600]; int o = snprintf(line, sizeof line, "  %s=%p", name, v);
    if (readable(v, 64)) {
        void **q = (void **)v;
        for (int i = 0; i < 8 && o < 540; i++) {
            unsigned char *x = (unsigned char *)q[i];
            if (x >= exe && x < exe + 0x10000000) o += snprintf(line + o, sizeof line - o, " [%d]=exe+%llx", i, (unsigned long long)(x - exe));
            else o += snprintf(line + o, sizeof line - o, " [%d]=%p", i, x);
        }
        unsigned char **vt = (unsigned char **)q[0];
        if ((unsigned char *)vt >= exe && (unsigned char *)vt < exe + 0x10000000 && readable(vt, 0x80))
            o += snprintf(line + o, sizeof line - o, " vt[9]=exe+%llx vt[6]=exe+%llx", (unsigned long long)(vt[9] - exe), (unsigned long long)(vt[6] - exe));
    }
    if (readable(v, 16) && readable(((void **)v)[0], 16)) {
        const wchar_t *w = (const wchar_t *)((void **)v)[0];
        char t[200]; int k = 0;
        while (k < 190 && readable(w + k, 2) && w[k] >= 32 && w[k] < 127) { t[k] = (char)w[k]; k++; }
        t[k] = 0;
        if (k >= 2 && o < 380) snprintf(line + o, sizeof line - o, " str=\"%s\"", t);
    }
    logf_("%s", line);
}
void *hook_trace(void *a, void *b, void *c, void *d)
{
    log_stack("traced function");
    static const char *nm[8] = {"rbx","rsi","rdi","rbp","r12","r13","r14","r15"};
    for (int i = 0; i < 8; i++) log_ptr(nm[i], g_regs[i]);
    log_ptr("rcx", a); log_ptr("rdx", b);
    if (GetEnvironmentVariableA("FNOFFLINE_TRACE_SKIP", NULL, 0)) { logf_("trace: call skipped"); return NULL; }
    return g_trace_orig(a, b, c, d);
}
static void install_trace(void)
{
    char v[64]; if (!GetEnvironmentVariableA("FNOFFLINE_TRACE_RVA", v, sizeof v)) return;
    unsigned long long rva = strtoull(v, NULL, 16); char *comma = strchr(v, ','); int len = comma ? atoi(comma + 1) : 0;
    if (!rva || len < 14 || len > 32) { logf_("trace: bad FNOFFLINE_TRACE_RVA '%s'", v); return; }
    unsigned char *target = (unsigned char *)GetModuleHandleW(NULL) + rva;
    unsigned char *tramp = (unsigned char *)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    memcpy(tramp, target, len);
    unsigned char back[14] = {0xFF,0x25,0,0,0,0}; *(unsigned long long *)(back + 6) = (unsigned long long)(target + len);
    memcpy(tramp + len, back, 14);
    g_trace_orig = (any4_fn)tramp;
    unsigned char jmp[14] = {0xFF,0x25,0,0,0,0}; *(unsigned long long *)(jmp + 6) = (unsigned long long)&trace_stub;
    DWORD old; VirtualProtect(target, 14, PAGE_EXECUTE_READWRITE, &old); memcpy(target, jmp, 14); VirtualProtect(target, 14, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, 14);
    logf_("trace: hooked exe+0x%llx (%d bytes)", rva, len);
}

/* FNOFFLINE_PATCHES=<rva>:<expected hex>:<new hex>[;...] : small code patches inside this process only.
   Each patch is applied only if the bytes at exe+rva are exactly <expected>, so a wrong build is never touched. */
static int hexbytes(const char *h, unsigned char *out, int max)
{
    int n = 0;
    while (h[0] && h[1] && n < max) { char t[3] = {h[0], h[1], 0}; out[n++] = (unsigned char)strtoul(t, NULL, 16); h += 2; }
    return n;
}
static void apply_patches(void)
{
    char v[1024]; if (!GetEnvironmentVariableA("FNOFFLINE_PATCHES", v, sizeof v)) return;
    unsigned char *exe = (unsigned char *)GetModuleHandleW(NULL);
    for (char *item = strtok(v, ";"); item; item = strtok(NULL, ";")) {
        char *a = strchr(item, ':'); if (!a) continue; *a++ = 0;
        char *b = strchr(a, ':'); if (!b) continue; *b++ = 0;
        unsigned long long rva = strtoull(item, NULL, 16);
        unsigned char exp[32], rep[32]; int ne = hexbytes(a, exp, 32), nr = hexbytes(b, rep, 32);
        unsigned char *t = exe + rva;
        if (!rva || !ne || !nr || nr > ne || !readable(t, ne)) { logf_("patch exe+%llx: bad entry", rva); continue; }
        if (memcmp(t, exp, ne) != 0) { logf_("patch exe+%llx: bytes differ, skipped", rva); continue; }
        DWORD old;
        if (!VirtualProtect(t, nr, PAGE_EXECUTE_READWRITE, &old)) { logf_("patch exe+%llx: protect failed", rva); continue; }
        memcpy(t, rep, nr); VirtualProtect(t, nr, old, &old); FlushInstructionCache(GetCurrentProcess(), t, nr);
        logf_("patch exe+%llx: applied (%d bytes)", rva, nr);
    }
}

/* FNOFFLINE_IOSTORE_UNSIGNED=1: let the build mount our own unsigned IoStore cosmetic containers (the ones under
   ~mods, pakchunk9900+), in memory and only in this process.
   IoStore builds (UE 4.26+/UE5) verify each container at mount in FIoStoreTocResource::Read:
       const bool bIsSigned = (Header.ContainerFlags & Signed);
       if (IsSigningEnabled() || bIsSigned) {
           if (!bIsSigned) return FIoStatus(SignatureError, "Missing signature");   // <- our containers die here
           ... parse the signature block ...
           if (IsSigningEnabled()) ValidateContainerSignature(...);                 // RSA check of signed ones
       }
   Fortnite binds a pak signing key, so IsSigningEnabled() is true and our unsigned container (ContainerFlags has
   only Indexed, no Signed) is rejected with "Missing signature". The minimal, safe fix is to retarget that ONE
   branch: `if (!bIsSigned) return "Missing signature"` becomes `if (!bIsSigned) goto <skip the signature block>`,
   jumping to the exact label the engine itself uses for an unsigned container when signing is disabled (the
   `test bl,bl; je <dir-index>` in the "signing disabled" path). Effect:
     - our unsigned container: the signature block is skipped, the container mounts (treated as unsigned);
     - a real signed container (bIsSigned true): the branch is not taken, so it is parsed AND still fully
       signature-validated, byte for byte as before. Only unsigned containers change behaviour, and every
       container the real build ships is signed, so in practice only our ~mods containers are affected.
   The site is found by its unique byte pattern (exactly 1 or nothing is touched), anchored on the L"Missing
   signature" string, not on any RVA, so the same code works across 4.26/UE5 IoStore builds.

   Pass 1: the wide string L"Missing signature" in a readable data page (exactly 1).
   Pass 2: the instruction that RIP-references that string (its disp32 points at it) (exactly 1) -> MEND.
   Pass 3: the `0F 84 rel32` (je) whose target lands in [MEND-24, MEND] (the SignatureError/"Missing signature"
           FIoStatus block just before the string load) (exactly 1) -> P (patch site), T (its current target).
   Pass 4: the `0F 84 relK  E9 relJ` (je then jmp) whose jmp-back target is P-4..P+2 (the engine's own
           "unsigned -> skip" branch) (exactly 1) -> K (the skip label).
   Patch: rewrite P's rel32 so the je lands on K instead of T. Verified against the real 17.50 exe:
   @ exe+0xf84a7a  0F84 77DA3102 (-> 0x32a24f7 Missing signature)  ->  0F84 0C010000 (-> 0xf84b8c skip). */
typedef struct { unsigned char *p; size_t n; } io_region;
static int io_regions(io_region *exe_r, io_region *rd_r, int max, int *nexe, int *nrd)
{
    unsigned char *base = (unsigned char *)GetModuleHandleW(NULL);
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    unsigned char *p = base, *end = base + nt->OptionalHeader.SizeOfImage;
    *nexe = *nrd = 0;
    while (p < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(p, &mbi, sizeof mbi)) break;
        unsigned char *rend = (unsigned char *)mbi.BaseAddress + mbi.RegionSize;
        if (rend > end) rend = end;
        DWORD prot = mbi.Protect & 0xFF;
        int rd = mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) &&
            (prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY ||
             prot == PAGE_READONLY || prot == PAGE_READWRITE || prot == PAGE_WRITECOPY);
        int ex = prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY;
        if (rd && rend - p > 16) {
            if (ex && *nexe < max) exe_r[(*nexe)++] = (io_region){p, (size_t)(rend - p)};
            if (*nrd < max) rd_r[(*nrd)++] = (io_region){p, (size_t)(rend - p)};
        }
        p = rend;
    }
    return *nexe > 0;
}

/* one RIP ref to an absolute target: a position whose next-4-bytes-end + int32 == target. Count and first (as end). */
static unsigned char *io_find_rip_ref(io_region *r, int nr, unsigned char *target, int *count)
{
    unsigned char *first = NULL; *count = 0;
    for (int s = 0; s < nr; s++) {
        unsigned char *b = r[s].p; size_t n = r[s].n;
        for (size_t i = 0; i + 4 <= n; i++) {
            unsigned char *endp = b + i + 4;
            if (endp + *(int *)(b + i) == target) { if (!first) first = endp; (*count)++; }
        }
    }
    return first;
}

/* the one `0F 84 rel32` (je) whose target is within [lo, hi]. Count and first (the 0F byte). */
static unsigned char *io_find_je(io_region *r, int nr, unsigned char *lo, unsigned char *hi, int *count)
{
    unsigned char *first = NULL; *count = 0;
    for (int s = 0; s < nr; s++) {
        unsigned char *b = r[s].p; size_t n = r[s].n;
        for (size_t i = 0; i + 6 <= n; i++) {
            if (b[i] != 0x0F || b[i + 1] != 0x84) continue;
            unsigned char *t = b + i + 6 + *(int *)(b + i + 2);
            if (t >= lo && t <= hi) { if (!first) first = b + i; (*count)++; }
        }
    }
    return first;
}

/* the one `0F 84 relK  E9 relJ` whose jmp-back target is within [lo, hi]. Count; outputs K (the je target). */
static unsigned char *io_find_skip(io_region *r, int nr, unsigned char *lo, unsigned char *hi, int *count)
{
    unsigned char *K = NULL; *count = 0;
    for (int s = 0; s < nr; s++) {
        unsigned char *b = r[s].p; size_t n = r[s].n;
        for (size_t i = 0; i + 11 <= n; i++) {
            if (b[i] != 0x0F || b[i + 1] != 0x84 || b[i + 6] != 0xE9) continue;
            unsigned char *je = b + i, *jmp = je + 6, *back = jmp + 5 + *(int *)(b + i + 7);
            if (back >= lo && back <= hi) { if (!K) K = je + 6 + *(int *)(b + i + 2); (*count)++; }
        }
    }
    return K;
}

/* Returns 1 patched (or already patched), -1 ambiguous/unsupported (logged, do not retry), 0 string not found yet. */
static int install_iostore_unsigned(int quiet)
{
    io_region exe_r[256], rd_r[256]; int nexe = 0, nrd = 0;
    if (!io_regions(exe_r, rd_r, 256, &nexe, &nrd)) return 0;

    static const unsigned char STR[] = {'M',0,'i',0,'s',0,'s',0,'i',0,'n',0,'g',0,' ',0,
                                        's',0,'i',0,'g',0,'n',0,'a',0,'t',0,'u',0,'r',0,'e',0, 0,0};
    unsigned char *S = NULL; int nS = 0;
    for (int s = 0; s < nrd && nS < 2; s++) {
        unsigned char *b = rd_r[s].p; size_t n = rd_r[s].n;
        for (size_t i = 0; i + sizeof STR <= n; i += 2)
            if (memcmp(b + i, STR, sizeof STR) == 0) { if (!S) S = b + i; nS++; break; }
    }
    if (!S) return 0;                                   /* retry: pages may not be mapped yet */
    if (nS != 1) { if (!quiet) logf_("iostore: L\"Missing signature\" appears %d times (need 1); nothing patched", nS); return -1; }

    int c; unsigned char *mend = io_find_rip_ref(exe_r, nexe, S, &c);
    if (!mend || c != 1) { if (!quiet) logf_("iostore: %d refs to the string (need 1); nothing patched", c); return c ? -1 : 0; }

    unsigned char *P = io_find_je(exe_r, nexe, mend - 24, mend, &c);
    if (!P || c != 1) { if (!quiet) logf_("iostore: %d je into the missing-signature block (need 1); nothing patched", c); return -1; }
    unsigned char *T = P + 6 + *(int *)(P + 2);         /* current target: the Missing-signature block */

    unsigned char *K = io_find_skip(exe_r, nexe, P - 4, P + 2, &c);
    if (!K || c != 1) { if (!quiet) logf_("iostore: %d skip branches (need 1); nothing patched", c); return -1; }

    if (T == K) { logf_("iostore: already treating unsigned containers as unsigned (nothing to do)"); return 1; }
    unsigned char *exe = (unsigned char *)GetModuleHandleW(NULL);
    int newrel = (int)(K - (P + 6));
    DWORD old;
    if (!VirtualProtect(P + 2, 4, PAGE_EXECUTE_READWRITE, &old)) { logf_("iostore: VirtualProtect failed"); return -1; }
    *(int *)(P + 2) = newrel;
    VirtualProtect(P + 2, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), P, 6);
    logf_("OK: iostore signature check neutralised at exe+0x%llx (unsigned containers -> skip, like signing-off; "
          "signed containers still validated). was je exe+0x%llx, now je exe+0x%llx",
          (unsigned long long)(P - exe), (unsigned long long)(T - exe), (unsigned long long)(K - exe));
    return 1;
}

static DWORD WINAPI iostore_retry_thread(LPVOID p)
{
    (void)p;
    for (int i = 1; i <= 480; i++) {
        Sleep(250);
        int r = install_iostore_unsigned(1);
        if (r == 1) { logf_("(iostore check found after %d.%02d s)", i / 4, (i % 4) * 25); return 0; }
        if (r < 0) { install_iostore_unsigned(0); return 0; }
    }
    logf_("iostore: L\"Missing signature\" not found in 120 s; nothing patched");
    return 0;
}

static void install_iostore(void)
{
    char t[8];
    if (!(GetEnvironmentVariableA("FNOFFLINE_IOSTORE_UNSIGNED", t, sizeof t) && t[0] == '1')) return;
    int r = install_iostore_unsigned(1);
    if (r == 1) return;
    if (r < 0) { install_iostore_unsigned(0); return; }
    logf_("iostore: signature check not in the code at load time: waiting");
    CloseHandle(CreateThread(NULL, 0, iostore_retry_thread, NULL, 0, NULL));
}

/* Older builds (seen on 3.6 and 14.30) ship their code encrypted on disk: the executable carries a second .text
   section (the unpacker) and the real code is decrypted in memory after the entry point, so at load time the scan
   finds nothing. Then the scan is repeated every 250 ms (up to 120 s) until the decrypted code contains exactly one
   curl_easy_setopt, validated by the same signature. Nothing is patched while there are 0 or several matches. */
static DWORD WINAPI install_retry_thread(LPVOID p)
{
    (void)p;
    for (int i = 1; i <= 480; i++) {
        Sleep(250);
        int r = install_quiet(1);
        if (r == 1) { logf_("(found after %d.%02d s: the code was decrypted at run time)", i / 4, (i % 4) * 25); return 0; }
        if (r < 0) { install_quiet(0); return 0; } /* several matches: log it and stop, never guess */
    }
    logf_("UNSUPPORTED: curl_easy_setopt not found in 120 s; nothing patched");
    return 0;
}
static int install(void)
{
    int r = install_quiet(1);
    if (r == 1) return 1;
    if (r < 0) return install_quiet(0);
    logf_("curl_easy_setopt not in the code at load time: waiting for it to be decrypted");
    CloseHandle(CreateThread(NULL, 0, install_retry_thread, NULL, 0, NULL));
    return 0;
}

static int patch_named(HMODULE mod, const char *fn, void *hook, void **orig)
{
    int n = 0;
    unsigned char *base = (unsigned char *)mod;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return 0;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); d->Name; d++) {
        if (!d->OriginalFirstThunk) continue;
        IMAGE_THUNK_DATA64 *names = (IMAGE_THUNK_DATA64 *)(base + d->OriginalFirstThunk);
        IMAGE_THUNK_DATA64 *funcs = (IMAGE_THUNK_DATA64 *)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, funcs++) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME *ibn = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
            if (strcmp((const char *)ibn->Name, fn) != 0 || (void *)funcs->u1.Function == hook) continue;
            if (!*orig) *orig = (void *)funcs->u1.Function;
            DWORD old;
            if (VirtualProtect(&funcs->u1.Function, sizeof(ULONGLONG), PAGE_READWRITE, &old)) {
                funcs->u1.Function = (ULONGLONG)hook;
                VirtualProtect(&funcs->u1.Function, sizeof(ULONGLONG), old, &old);
                n++;
            }
        }
    }
    return n;
}

/* Replaces SHGetKnownFolderPath and SHGetFolderPathW in the import table of one module. Returns patched slots. */
static int patch_iat(HMODULE mod)
{
    return patch_named(mod, "SHGetKnownFolderPath", (void *)hook_known, (void **)&g_known_orig)
         + patch_named(mod, "SHGetFolderPathW", (void *)hook_folder, (void **)&g_folder_orig);
}

static HINSTANCE WINAPI hook_shexec(HWND, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, INT);
static BOOL WINAPI hook_shexecex(SHELLEXECUTEINFOW *);
typedef HINSTANCE (WINAPI *shexec_fn)(HWND, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, INT);
typedef BOOL (WINAPI *shexecex_fn)(SHELLEXECUTEINFOW *);
static shexec_fn g_shexec_orig; static shexecex_fn g_shexecex_orig;

static int patch_all_modules(void)
{
    HMODULE mods[1024]; DWORD needed = 0; int n = 0;
    /* K32EnumProcessModules lives in kernel32: no LoadLibrary under the loader lock */
    typedef BOOL (WINAPI *enum_fn)(HANDLE, HMODULE *, DWORD, LPDWORD);
    enum_fn en = (enum_fn)GetProcAddress(GetModuleHandleA("kernel32.dll"), "K32EnumProcessModules");
    if (!en || !en(GetCurrentProcess(), mods, sizeof mods, &needed)) return 0;
    for (DWORD i = 0; i < needed / sizeof(HMODULE) && i < 1024; i++) {
        n += patch_iat(mods[i]);
        if (g_trace_exit) patch_named(mods[i], "PostQuitMessage", (void *)hook_pqm, (void **)&g_pqm_orig);
        {
            patch_named(mods[i], "ShellExecuteW", (void *)hook_shexec, (void **)&g_shexec_orig);
            patch_named(mods[i], "ShellExecuteExW", (void *)hook_shexecex, (void **)&g_shexecex_orig);
        }
    }
    return n;
}

/* FNOFFLINE_TRACE_CRASH=1 (diagnostics): log access violations (first chance, max 40) with module+offset and a short stack. */
static volatile LONG g_av_count;
static LONG WINAPI av_logger(EXCEPTION_POINTERS *e)
{
    DWORD code = e->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_STACK_OVERFLOW && code != 0xC0000409) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedIncrement(&g_av_count) > 40) return EXCEPTION_CONTINUE_SEARCH;
    void *at = e->ExceptionRecord->ExceptionAddress;
    HMODULE m = NULL; char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)at, &m) && m)
        GetModuleFileNameA(m, name, MAX_PATH);
    const char *base = strrchr(name, '\\'); base = base ? base + 1 : name;
    logf_("EXCEPTION %08lx tid %lu at %s+0x%llx (%s %p)", code, GetCurrentThreadId(), base,
          (unsigned long long)((unsigned char *)at - (unsigned char *)m),
          e->ExceptionRecord->NumberParameters > 1 ? (e->ExceptionRecord->ExceptionInformation[0] ? "write" : "read") : "-",
          e->ExceptionRecord->NumberParameters > 1 ? (void *)e->ExceptionRecord->ExceptionInformation[1] : NULL);
    if (g_av_count <= 6) {
        typedef HRESULT (WINAPI *gtd_fn)(HANDLE, PWSTR *);
        gtd_fn gtd = (gtd_fn)GetProcAddress(GetModuleHandleA("kernelbase.dll"), "GetThreadDescription");
        PWSTR desc = NULL;
        if (gtd && SUCCEEDED(gtd(GetCurrentThread(), &desc)) && desc) { logf_("  thread \"%ls\"", desc); LocalFree(desc); }
        /* the caller: return addresses on the faulting thread's stack that fall inside a module */
        ULONG_PTR *sp = (ULONG_PTR *)e->ContextRecord->Rsp;
        logf_("  rip %p rsp %p rcx %p rdx %p", (void *)e->ContextRecord->Rip, sp, (void *)e->ContextRecord->Rcx, (void *)e->ContextRecord->Rdx);
        int shown = 0;
        for (int i = 0; i < 256 && shown < 10 && readable(sp + i, 8); i++) {
            HMODULE rm = NULL; char rn[MAX_PATH];
            if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)sp[i], &rm) || !rm) continue;
            GetModuleFileNameA(rm, rn, MAX_PATH);
            const char *b2 = strrchr(rn, '\\'); b2 = b2 ? b2 + 1 : rn;
            logf_("  [rsp+%x] %s+0x%llx", i * 8, b2, (unsigned long long)(sp[i] - (ULONG_PTR)rm));
            shown++;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* With FNOFFLINE_TRACE_CRASH=1 also log every DLL load/unload (base, size): tells whether a bad address was a freed module. */
typedef struct { USHORT Length, MaximumLength; PWSTR Buffer; } fn_ustr;
typedef struct { ULONG Flags; const fn_ustr *FullDllName, *BaseDllName; PVOID DllBase; ULONG SizeOfImage; } fn_dllnote;
static VOID CALLBACK dll_note(ULONG reason, const fn_dllnote *d, PVOID ctx)
{
    (void)ctx;
    logf_("%s %.*ls %p+%lx", reason == 1 ? "dll load" : "dll UNLOAD", d->BaseDllName->Length / 2, d->BaseDllName->Buffer, d->DllBase, d->SizeOfImage);
}

/* What the game asks the Windows shell to open (URLs, protocols, files), always logged. Never opened: the Epic launcher
   protocol (3.x-15.x builds ask com.epicgames.launcher://apps/<build>?action=verify at every start, which would open the
   real Epic Games Launcher on the user's PC) and, with FNOFFLINE_BLOCK_SHELL=1, anything. */
static int g_block_shell;
static int shell_blocked(const wchar_t *f) { return g_block_shell || (f && !_wcsnicmp(f, L"com.epicgames.launcher:", 23)); }
static HINSTANCE WINAPI hook_shexec(HWND h, LPCWSTR op, LPCWSTR file, LPCWSTR params, LPCWSTR dir, INT show)
{
    int b = shell_blocked(file);
    logf_("ShellExecuteW %ls \"%ls\" %ls%s", op ? op : L"(open)", file ? file : L"", params ? params : L"", b ? " (blocked)" : "");
    if (b) return (HINSTANCE)(INT_PTR)42;
    return g_shexec_orig(h, op, file, params, dir, show);
}
static BOOL WINAPI hook_shexecex(SHELLEXECUTEINFOW *i)
{
    int b = shell_blocked(i ? i->lpFile : NULL);
    logf_("ShellExecuteExW %ls \"%ls\" %ls%s", i && i->lpVerb ? i->lpVerb : L"(open)", i && i->lpFile ? i->lpFile : L"", i && i->lpParameters ? i->lpParameters : L"", b ? " (blocked)" : "");
    if (b) { if (i) i->hInstApp = (HINSTANCE)(INT_PTR)42; return TRUE; }
    return g_shexecex_orig(i);
}

/* Modules loaded later (EOS SDK, plugins) get patched too: rescan for a while. */
static DWORD WINAPI rescan_thread(LPVOID p)
{
    (void)p;
    for (int i = 0; i < 120; i++) { Sleep(1000); int n = patch_all_modules(); if (n) logf_("sandbox: patched %d more import slots", n); }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)inst; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        { /* very first sign of life, no CRT: marker next to the log (debug) */
            char m[MAX_PATH]; DWORD n = GetEnvironmentVariableA("FNOFFLINE_LOG", m, MAX_PATH - 8);
            if (n && n < MAX_PATH - 8) { lstrcatA(m, ".start"); HANDLE h = CreateFileA(m, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL); if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
        }
        InitializeCriticalSection(&g_log_lock);
        char port[16] = "3551";
        GetEnvironmentVariableA("FNOFFLINE_PORT", port, sizeof port);
        int p = atoi(port);
        if (p <= 0 || p > 65535) p = 3551;
        snprintf(g_prefix, sizeof g_prefix, "http://127.0.0.1:%d", p);
        if (!GetEnvironmentVariableA("FNOFFLINE_LOG", g_log, sizeof g_log)) g_log[0] = 0;
        logf_("loaded into pid %lu", GetCurrentProcessId());
        /* Hook synchronously: the game's main thread is running, but its first HTTP request comes seconds later,
           and patching 14 bytes is atomic enough for a function nobody is executing yet. */
        { char t[8]; g_hwbp = GetEnvironmentVariableA("FNOFFLINE_CURL_HOOK", t, sizeof t) && !_stricmp(t, "hwbp"); }
        { char t[8]; if (!(GetEnvironmentVariableA("FNOFFLINE_NO_CURL_HOOK", t, sizeof t) && t[0] == '1')) install(); else logf_("curl hook disabled (FNOFFLINE_NO_CURL_HOOK=1, diagnostics)"); }
        install_trace();
        apply_patches();
        install_iostore();
        { char t[8]; g_trace_exit = GetEnvironmentVariableA("FNOFFLINE_TRACE_EXIT", t, sizeof t) && t[0] == '1'; }
        { char t[8]; g_block_shell = GetEnvironmentVariableA("FNOFFLINE_BLOCK_SHELL", t, sizeof t) && t[0] == '1';
        }
        { char t[8]; if (GetEnvironmentVariableA("FNOFFLINE_TRACE_CRASH", t, sizeof t) && t[0] == '1') { AddVectoredExceptionHandler(1, av_logger);
            typedef LONG (NTAPI *reg_fn)(ULONG, void *, PVOID, PVOID *);
            reg_fn reg = (reg_fn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "LdrRegisterDllNotification");
            static PVOID cookie; if (reg) reg(0, (void *)dll_note, NULL, &cookie); } }
        if (GetEnvironmentVariableW(L"FNOFFLINE_SANDBOX", g_sandbox, MAX_PATH)) {
            if (!g_known_orig) {
                HMODULE sh = GetModuleHandleA("shell32.dll");
                if (sh) g_known_orig = (known_fn)GetProcAddress(sh, "SHGetKnownFolderPath");
            }
            int n = patch_all_modules();
            logf_("sandbox: %ls (%d import slots patched)", g_sandbox, n);
            CloseHandle(CreateThread(NULL, 0, rescan_thread, NULL, 0, NULL));
        } else g_sandbox[0] = 0;
    }
    else if (reason == DLL_THREAD_ATTACH && g_hwbp && g_bp_addr) arm_self(); /* every new thread starts armed */
    return TRUE;
}
