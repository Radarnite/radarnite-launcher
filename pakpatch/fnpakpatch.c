/*
 * fnpakpatch.dll - the two patches of LargePakPatch.dll, made to work on 14.30 and never closing the game.
 *
 * LargePakPatch (Memcury + MinHook) finds the code that uses two strings and turns it into no-ops:
 *   - the function that uses L"UnsafeEnvironment_Title" (the "unsafe environment" check) -> returns 0
 *   - the 2nd call after L"Memory pools allocations failed, exiting..." (the exit)       -> returns at once
 * On 14.30 it found the strings but not the function start (it looks for 4C 8B DC / 40 55 before the reference), then
 * showed "Addresses::Init()" and closed the game. Here the function start comes from the exe's own unwind table
 * (.pdata), the scan is repeated until the code is decrypted (14.x decrypts it after the entry point) and nothing is
 * patched unless each string has exactly one user. Every step goes to <FNOFFLINE_SANDBOX>\pakpatch.log.
 *
 * Injected by the launcher after fnredirect.dll confirmed (Session.LoadPakPatch).
 * Build: zig cc -target x86_64-windows-gnu -O2 -shared -s -o fnpakpatch.dll fnpakpatch.c
 */
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static char g_log[MAX_PATH];

static void logf_(const char *fmt, ...)
{
    if (!g_log[0]) return;
    FILE *f = fopen(g_log, "a");
    if (!f) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(f, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a);
    fputc('\n', f); fclose(f);
}

typedef struct { unsigned char *p; SIZE_T n; } span;
static unsigned char *g_base;
static span g_text, g_img;

static int sections(void)
{
    g_base = (unsigned char *)GetModuleHandleW(NULL);
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(g_base + ((IMAGE_DOS_HEADER *)g_base)->e_lfanew);
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    g_img.p = g_base; g_img.n = nt->OptionalHeader.SizeOfImage;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (!memcmp(s[i].Name, ".text", 6)) { g_text.p = g_base + s[i].VirtualAddress; g_text.n = s[i].Misc.VirtualSize; return 1; } /* the first .text is the game code */
    return 0;
}

/* first copy of a UTF-16 string outside .text */
static unsigned char *find_wstr(const wchar_t *w)
{
    SIZE_T len = wcslen(w) * 2 + 2;
    for (unsigned char *p = g_text.p + g_text.n; p + len <= g_img.p + g_img.n; p += 2) {
        MEMORY_BASIC_INFORMATION mi;
        if (((ULONG_PTR)p & 0xFFF) == 0 && VirtualQuery(p, &mi, sizeof mi) && (mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))) {
            p = (unsigned char *)mi.BaseAddress + mi.RegionSize - 2; continue;
        }
        if (!memcmp(p, w, len)) return p;
    }
    return NULL;
}

/* every "lea r64, [rip+x]" in .text whose target is s (up to max of them) */
static int find_refs(unsigned char *s, unsigned char **out, int max)
{
    int n = 0;
    for (unsigned char *p = g_text.p; p + 7 <= g_text.p + g_text.n; p++) {
        if ((p[0] != 0x48 && p[0] != 0x4C) || p[1] != 0x8D || (p[2] & 0xC7) != 0x05) continue;
        if (p + 7 + *(INT32 *)(p + 3) != s) continue;
        if (n < max) out[n] = p;
        n++;
    }
    return n;
}

/* start of the function containing pc (follows chained unwind info to the primary entry) */
static unsigned char *func_start(unsigned char *pc)
{
    DWORD64 base;
    PRUNTIME_FUNCTION f = RtlLookupFunctionEntry((DWORD64)pc, &base, NULL);
    for (int i = 0; f && i < 16; i++) {
        unsigned char *ui = (unsigned char *)base + f->UnwindData;
        if (!((ui[0] >> 3) & UNW_FLAG_CHAININFO)) return (unsigned char *)base + f->BeginAddress;
        f = (PRUNTIME_FUNCTION)(ui + 4 + ((ui[2] + 1) & ~1) * 2);
    }
    return NULL;
}

static void hex(const unsigned char *p, int n, char *out)
{
    for (int i = 0; i < n; i++) sprintf(out + i * 3, "%02X ", p[i]);
    out[n ? n * 3 - 1 : 0] = 0;
}

static int write_code(unsigned char *at, const unsigned char *code, SIZE_T n)
{
    DWORD old;
    if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy(at, code, n); VirtualProtect(at, n, old, &old); FlushInstructionCache(GetCurrentProcess(), at, n);
    return 1;
}

static DWORD WINAPI worker(LPVOID unused)
{
    (void)unused;
    static const unsigned char RET0[] = { 0x31, 0xC0, 0xC3 }, RET[] = { 0xC3 };
    char b[64];
    if (!sections()) { logf_("FAIL: no .text section"); return 0; }
    unsigned char *s_unsafe = find_wstr(L"UnsafeEnvironment_Title");
    unsigned char *s_pools = find_wstr(L"Memory pools allocations failed, exiting...");
    logf_("strings: UnsafeEnvironment_Title %s, Memory pools %s", s_unsafe ? "found" : "MISSING", s_pools ? "found" : "MISSING");
    unsigned char *r_unsafe[4], *r_pools[4];
    int nu = 0, np = 0, i;
    for (i = 0; i < 240; i++) { /* up to 120 s: 14.x decrypts its code after the entry point */
        nu = s_unsafe ? find_refs(s_unsafe, r_unsafe, 4) : 0;
        np = s_pools ? find_refs(s_pools, r_pools, 4) : 0;
        if ((!s_unsafe || nu) && (!s_pools || np)) break;
        Sleep(500);
    }
    logf_("references after %d.%d s: unsafe %d, pools %d", i / 2, (i % 2) * 5, nu, np);

    if (nu == 1) {
        unsigned char *f = func_start(r_unsafe[0]);
        if (!f) logf_("unsafe: no unwind entry for exe+%llx: not patched", (unsigned long long)(r_unsafe[0] - g_base));
        else {
            hex(f, 12, b);
            int ok = write_code(f, RET0, sizeof RET0);
            logf_("unsafe: function exe+%llx (ref exe+%llx) [%s] -> return 0: %s", (unsigned long long)(f - g_base),
                  (unsigned long long)(r_unsafe[0] - g_base), b, ok ? "patched" : "FAILED");
        }
    } else logf_("unsafe: %d references: not patched", nu);

    if (np == 1) {
        unsigned char *p = r_pools[0] + 1, *end = p + 0x800, *target = NULL; /* byte scan from ref+1, as LargePakPatch */
        int calls = 0;
        for (; p + 5 <= end; p++)
            if (*p == 0xE8 && ++calls == 2) { target = p + 5 + *(INT32 *)(p + 1); break; }
        if (!target || target < g_text.p || target >= g_text.p + g_text.n || func_start(target) != target)
            logf_("pools: 2nd call after exe+%llx is not a function start: not patched", (unsigned long long)(r_pools[0] - g_base));
        else {
            hex(target, 12, b);
            int ok = write_code(target, RET, sizeof RET);
            logf_("pools: call at exe+%llx -> exe+%llx [%s] -> return: %s", (unsigned long long)(p - g_base),
                  (unsigned long long)(target - g_base), b, ok ? "patched" : "FAILED");
        }
    } else logf_("pools: %d references: not patched", np);
    logf_("done");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    (void)r;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        char sb[MAX_PATH];
        DWORD n = GetEnvironmentVariableA("FNOFFLINE_SANDBOX", sb, sizeof sb);
        if (n && n < sizeof sb - 16) snprintf(g_log, sizeof g_log, "%s\\pakpatch.log", sb);
        logf_("fnpakpatch loaded into pid %lu", GetCurrentProcessId());
        HANDLE t = CreateThread(NULL, 0, worker, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
