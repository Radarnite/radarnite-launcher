/*
 * fnmods.dll - the mods of Fortnite 14.30, one DLL with modules (injected by the launcher into the game client only:
 * Session.LoadMods, patches\fnmods.dll; idle if it ends up in the -nullrhi match server). It changes no byte of the
 * game's code (14.x checks its own code; see fnredirect.c): it reads and drives the engine through its own reflection
 * and ProcessEvent (fnengine.c).
 *
 * Modules (each its own file, #included here; mods.json turns each on or off, fnconfig.c):
 *   body      fnbody.c      body sliders of fn-port outfits (morph targets from their style values)
 *   ragdoll   fnragdoll.c   ragdoll deaths instead of the digitize effect
 *   console   fnconsole.c   the engine console (Shipping builds have none)
 *   settings  fnsettings.c  field of view, frame rate cap, post effects, slow motion, fly
 *   cheats    fncheats.c    aim assist, teleports (crosshair, map pin, random POI), bot spawns, god mode (match only)
 *   menu      fnmenu.c      the in-game menu on F8 for the settings and the console
 *   test      fntest.c      developer test hooks (environment variables only; the launcher never sets them)
 * The host:
 *   - finds the engine once for all of them (object array, ProcessEvent, name pool) on a worker thread, then runs the
 *     modules on the game thread, from a WH_GETMESSAGE hook on the thread that owns the game window, woken about once a
 *     frame (16 ms) by a posted thread message (UE pumps messages once per frame with PeekMessage); the same hook sees
 *     the game window's key messages first (the menu's keys);
 *   - runs every module's tick under its own fault guard: a fault in this DLL's code inside one module turns that module
 *     off for the session, the others and the game go on (FNMODS_TEST_FAULT=<module> makes one fault on purpose);
 *   - logs, once in the lobby and once in the first match, a table of every class, property and function each module
 *     needs and whether 14.30 has it;
 *   - reads mods.json (FNMODS_CONFIG) at start and again whenever the launcher saves it.
 * Every step is logged to <FNOFFLINE_SANDBOX>\fnmods.log.
 *
 * Build: zig cc -target x86_64-windows-gnu -O2 -shared -s -o fnmods.dll fnmods.c -luser32
 */
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <wchar.h>

static void test_on_death(void *pawn, int ragdoll);
static DWORD g_game_tid;   /* the game thread (set once its window is found) */

#include "fnengine.c"
#include "fnconfig.c"
#include "fnbody.c"
#include "fnragdoll.c"
#include "fnconsole.c"
#include "fnsettings.c"
#include "fncheats.c"
#include "fnmenu.c"
#include "fntest.c"

/* ---------------------------------------------------------------- modules */
static const need BODY_NEEDS[] = {
    { 'c', "SkeletalMeshComponent" }, { 'c', "MaterialInstanceDynamic" }, { 'p', "SkinnedMeshComponent.SkeletalMesh" },
    { 'p', "MeshComponent.OverrideMaterials" }, { 'p', "SceneComponent.AttachParent" }, { 'p', "SceneComponent.RelativeScale3D" },
    { 'p', "SkeletalMesh.MorphTargets" }, { 'p', "MaterialInstance.ScalarParameterValues" }, { 'p', "ScalarParameterValue.ParameterValue", 1 },
    { 'f', "SkeletalMeshComponent.SetMorphTarget" }, { 'f', "SkeletalMeshComponent.GetMorphTarget", 1 }, { 'f', "SceneComponent.SetRelativeScale3D" },
};
static const need RAGDOLL_NEEDS[] = {
    { 'p', "FortPawn.bIsDying" }, { 'p', "FortPawn.bIsHiddenForDeath" }, { 'f', "FortPawn.OnDeathPlayEffects" },
    { 'f', "PlayerPawn_Athena_C.OnDeathPlayEffects", 1 }, { 'p', "Character.Mesh" }, { 'p', "Character.CapsuleComponent" },
    { 'f', "PrimitiveComponent.SetSimulatePhysics" }, { 'f', "SkeletalMeshComponent.SetAllBodiesSimulatePhysics" },
    { 'f', "PrimitiveComponent.SetCollisionProfileName" }, { 'f', "PrimitiveComponent.SetCollisionEnabled" },
    { 'f', "PrimitiveComponent.AddImpulse", 1 }, { 'f', "CharacterMovementComponent.DisableMovement", 1 },
    { 'f', "KismetStringLibrary.Conv_StringToName" },
};
static void body_kill(void) { g_body_off = 1; }
static void ragdoll_kill(void) { g_rd_mode = RD_OFF; }
static void console_kill(void) { CN.state = -1; }
static void settings_kill(void) { SN.state = -1; }
static void cheats_kill(void) { CH.state = -1; g_rmb = 0; }
static void menu_kill(void) { MN.dead = 1; g_menu_open = 0; }
static void test_kill(void) { g_mt = 0; g_join_flag[0] = 0; g_shots = 0; }
static int on_body(void) { return g_cfg.body; }
static int on_ragdoll(void) { return g_cfg.ragdoll; }
static int on_always(void) { return 1; }
static void tests_tick(void) { test_tick(); mt_tick(); }

typedef struct {
    const char *id;
    int (*on)(void);            /* ticked while it returns 1 (console, settings and menu tick always: off is a state they undo) */
    void (*tick)(void);
    void (*kill)(void);         /* after a fault: whatever the module left hooked stops acting */
    const need *needs; int nneeds;
    int dead;
} module;
#define NEEDS(a) a, (int)(sizeof a / sizeof a[0])
static module g_mods[] = {
    { "body", on_body, body_tick, body_kill, NEEDS(BODY_NEEDS), 0 },
    { "ragdoll", on_ragdoll, ragdoll_tick, ragdoll_kill, NEEDS(RAGDOLL_NEEDS), 0 },
    { "console", on_always, console_tick, console_kill, NEEDS(CONSOLE_NEEDS), 0 },
    { "settings", on_always, settings_tick, settings_kill, NEEDS(SETTINGS_NEEDS), 0 },
    { "cheats", on_always, cheats_tick, cheats_kill, NEEDS(CHEATS_NEEDS), 0 },
    { "menu", on_always, menu_tick, menu_kill, NEEDS(MENU_NEEDS), 0 },
    { "test", on_always, tests_tick, test_kill, NULL, 0, 0 },
};
#define NMODS ((int)(sizeof g_mods / sizeof g_mods[0]))
static char g_fault_mod[16]; static int g_fault_done;
static DWORD g_ready_tick;
static int *volatile g_null;
static void fault_now(void) { *g_null = 1; }   /* an access violation in this DLL's code (the store is real: g_null is volatile) */

/* the needs table: classes in one pass, then a line per need */
static void needs_report(const char *where)
{
    const char *names[128]; char buf[128][64]; int nn = 0;
    for (int m = 0; m < NMODS; m++)
        for (int k = 0; k < g_mods[m].nneeds && nn < 128; k++) {
            const char *s = g_mods[m].needs[k].name, *dot = strchr(s, '.');
            size_t l = dot ? (size_t)(dot - s) : strlen(s); if (l >= 64) l = 63;
            memcpy(buf[nn], s, l); buf[nn][l] = 0;
            int dup = 0; for (int j = 0; j < nn; j++) if (!strcmp(names[j], buf[nn])) dup = 1;
            if (!dup) { names[nn] = buf[nn]; nn++; }
        }
    void *cls[128]; find_types_any(names, cls, nn);
    logf_("engine functions the modules use (%s):", where);
    int found = 0, missing = 0, opt = 0;
    for (int m = 0; m < NMODS; m++)
        for (int k = 0; k < g_mods[m].nneeds; k++) {
            const need *nd = &g_mods[m].needs[k]; const char *dot = strchr(nd->name, '.');
            size_t l = dot ? (size_t)(dot - nd->name) : strlen(nd->name);
            void *c = NULL; for (int j = 0; j < nn; j++) if (strlen(names[j]) == l && !strncmp(names[j], nd->name, l)) c = cls[j];
            if (need_line(g_mods[m].id, nd, c)) found++; else if (nd->opt) opt++; else missing++;
        }
    logf_("engine functions (%s): %d found, %d missing, %d optional missing", where, found, missing, opt);
}

/* ---------------------------------------------------------------- game thread */
static UINT g_msg; static volatile LONG g_pending;
static LONG g_cfg_seen;
static void run_module(module *m)
{
    if (m->dead || !m->on()) return;
    if (g_fault_mod[0] && !g_fault_done && !strcmp(g_fault_mod, m->id) && GetTickCount() - g_ready_tick > 30000) {
        g_fault_done = 1;
        logf_("test: a fault on purpose in module %s", m->id);
        if (guarded(fault_now)) { m->dead = 1; m->kill(); logf_("FAULT in module %s: it is off for this session; the other modules go on", m->id); }
        return;
    }
    if (guarded(m->tick)) { m->dead = 1; m->kill(); logf_("FAULT in module %s (reading engine memory): it is off for this session; the other modules go on", m->id); }
}
static void tick(void)
{
    if (g_cfg_gen != g_cfg_seen) {
        g_cfg_seen = g_cfg_gen;
        EnterCriticalSection(&g_cfg_lock); g_cfg = g_cfg_file; LeaveCriticalSection(&g_cfg_lock);
    }
    /* the table of what the modules need: in the lobby once a player controller is there, and in the first match */
    static int lobby_done, match_done; static DWORD at; static void *pc_seen;
    if (!lobby_done || !match_done) {
        DWORD now = GetTickCount();
        void *pc = local_pc();
        if (pc && pc != pc_seen) { pc_seen = pc; at = now + (lobby_done ? 20000 : 5000); }
        if (pc && at && now >= at) {
            at = 0;
            if (!lobby_done) { lobby_done = 1; needs_report("lobby"); }
            else if (!SA.standalone) { match_done = 1; needs_report("match"); }
        }
    }
    for (int i = 0; i < NMODS; i++) run_module(&g_mods[i]);
}

static LRESULT CALLBACK getmsg(int code, WPARAM wp, LPARAM lp)
{
    MSG *m = (MSG *)lp;
    if (code == HC_ACTION && wp == PM_REMOVE && m) {
        if (m->message == g_msg && !m->hwnd) {
            tick();
            m->message = WM_NULL;
            InterlockedExchange(&g_pending, 0);
        } else if (m->message == WM_KEYDOWN || m->message == WM_KEYUP || m->message == WM_SYSKEYDOWN || m->message == WM_SYSKEYUP) {
            int down = m->message == WM_KEYDOWN || m->message == WM_SYSKEYDOWN;
            if (m->wParam == VK_SPACE) g_key_up = down;
            if (m->wParam == VK_CONTROL || m->wParam == VK_LCONTROL) g_key_down = down;
            if (menu_key(m->message, m->wParam) || cheats_key(m->message, m->wParam)) m->message = WM_NULL;
        } else if (m->message == WM_RBUTTONDOWN || m->message == WM_RBUTTONUP) {
            cheats_key(m->message, m->wParam);
        }
    }
    return CallNextHookEx(NULL, code, wp, lp);
}

typedef struct { DWORD pid; HWND w; } findw;
static BOOL CALLBACK enum_cb(HWND w, LPARAM lp)
{
    findw *f = (findw *)lp; DWORD pid = 0; char cls[64];
    GetWindowThreadProcessId(w, &pid);
    if (pid == f->pid && GetClassNameA(w, cls, sizeof cls) && !strcmp(cls, "UnrealWindow")) { f->w = w; return FALSE; }
    return TRUE;
}

/* ---------------------------------------------------------------- setup (worker thread) */
static DWORD WINAPI worker(LPVOID unused)
{
    (void)unused;
    cfg_poll();
    cfg_log("start", &g_cfg_file);
    g_cfg = g_cfg_file; g_cfg_seen = g_cfg_gen;
    body_init();
    ragdoll_init();
    test_init();
    mt_init();
    DWORD n = GetEnvironmentVariableA("FNMODS_TEST_FAULT", g_fault_mod, sizeof g_fault_mod);
    if (!n || n >= sizeof g_fault_mod) g_fault_mod[0] = 0; else logf_("test: module %s will fault once, 30 s after the hook", g_fault_mod);
    int i;
    for (i = 0; i < 600 && !g_objs; i++) { scan(PAT_OBJS, "xxx????xxxxxxxx", sizeof PAT_OBJS, objs_cb, NULL, NULL); if (!g_objs) Sleep(500); }
    if (!g_objs) { logf_("FAIL: GUObjectArray not found after %d s", i / 2); return 0; }
    logf_("GUObjectArray at exe+0x%llx (%d objects) after %d s", (unsigned long long)((u8 *)g_objs - g_base), g_objs->num, i / 2);
    u8 *pe = NULL;
    for (i = 0; i < 120 && !pe; i++) { pe = find_process_event(); if (!pe) Sleep(1000); }
    if (!pe || !in_vtable(pe)) { logf_("FAIL: ProcessEvent not found or not in a vtable (%p)", pe); return 0; }
    g_pe = (process_event_fn)pe;
    logf_("ProcessEvent at exe+0x%llx", (unsigned long long)(pe - g_base));
    for (i = 0; i < 120 && !g_name_blocks; i++) { g_name_blocks = find_name_pool(); if (!g_name_blocks) { if (!i) pool_diag(); Sleep(1000); } }
    if (!g_name_blocks) { logf_("FAIL: name pool not found"); return 0; }
    logf_("name pool blocks at exe+0x%llx (entry length shift %d, %s)", (unsigned long long)((u8 *)g_name_blocks - g_base), g_len_shift,
          g_name_enc ? "strings XOR-encoded" : "plain strings");
    if (g_name_enc) logf_("name strings: k0 0x%02x, step %u", g_name_k0, g_name_step);
    findw f = { GetCurrentProcessId(), NULL };
    for (i = 0; i < 1200 && !f.w; i++) { EnumWindows(enum_cb, (LPARAM)&f); if (!f.w) Sleep(500); }
    if (!f.w) { logf_("FAIL: no game window"); return 0; }
    g_game_hwnd = f.w;
    g_game_tid = GetWindowThreadProcessId(f.w, NULL);
    g_msg = RegisterWindowMessageA("RadarniteFnModsTick");
    HHOOK h = SetWindowsHookExW(WH_GETMESSAGE, getmsg, g_self, g_game_tid);
    if (!h) { logf_("FAIL: SetWindowsHookEx %lu", GetLastError()); return 0; }
    g_ready_tick = GetTickCount();
    logf_("game thread %lu hooked (message 0x%x); waking it every 16 ms", g_game_tid, g_msg);
    DWORD posted = 0;
    for (unsigned k = 1; ; k++) {
        Sleep(16);   /* about once a frame: a slider drag shows on the body in the same frame the game writes it */
        if (k % 64 == 0) cfg_poll();   /* about once a second: mods.json saved by the launcher while the game runs */
        if (InterlockedCompareExchange(&g_pending, 1, 0) == 0) {
            if (!PostThreadMessageA(g_game_tid, g_msg, 0, 0)) { InterlockedExchange(&g_pending, 0); if (!IsWindow(f.w)) break; }
            else posted++;
        }
    }
    UnhookWindowsHookEx(h);
    logf_("stopped after %lu ticks (faults %ld)", posted, g_faults);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    (void)r;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        g_self = h;
        InitializeCriticalSection(&g_log_lock);
        InitializeCriticalSection(&g_cfg_lock);
        cfg_defaults(&g_cfg_file);
        char sb[MAX_PATH];
        DWORD n = GetEnvironmentVariableA("FNOFFLINE_SANDBOX", sb, sizeof sb);
        if (n && n < sizeof sb - 16) snprintf(g_log, sizeof g_log, "%s\\fnmods.log", sb);
        n = GetEnvironmentVariableA("FNMODS_CONFIG", g_cfg_path, sizeof g_cfg_path);
        if (!n || n >= sizeof g_cfg_path) g_cfg_path[0] = 0;
        const char *cmd = GetCommandLineA();
        if (cmd && strstr(cmd, "-nullrhi")) { logf_("fnmods: match server process (-nullrhi): nothing to draw, not started"); return TRUE; }
        /* one copy per process (the same code loaded under two names would scale Height twice); the old fnbody.dll's
           name too, so neither runs beside the other */
        char mx[64]; snprintf(mx, sizeof mx, "Local\\RadarniteFnBody-%lu", GetCurrentProcessId());
        if (CreateMutexA(NULL, FALSE, mx) && GetLastError() == ERROR_ALREADY_EXISTS) { logf_("fnmods: already running in this process (another copy): this one stays idle"); return TRUE; }
        g_base = (u8 *)GetModuleHandleW(NULL);
        IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(g_base + ((IMAGE_DOS_HEADER *)g_base)->e_lfanew);
        g_size = nt->OptionalHeader.SizeOfImage;
        IMAGE_NT_HEADERS64 *me = (IMAGE_NT_HEADERS64 *)((u8 *)h + ((IMAGE_DOS_HEADER *)h)->e_lfanew);
        g_self_lo = (u8 *)h; g_self_hi = (u8 *)h + me->OptionalHeader.SizeOfImage;
        AddVectoredExceptionHandler(1, guard_handler);
        logf_("fnmods loaded into pid %lu (config %s)", GetCurrentProcessId(), g_cfg_path[0] ? g_cfg_path : "none: defaults");
        HANDLE t = CreateThread(NULL, 0, worker, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
