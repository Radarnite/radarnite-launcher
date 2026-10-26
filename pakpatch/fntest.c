/*
 * fntest.c - developer test hooks of fnmods.dll (#included by fnmods.c), for runs of the game in the background where
 * nobody can click: never active unless their environment variables are set, and the launcher never sets them.
 *
 *   FNTEST_JOIN=<file>   once <file> exists (the test runner creates it when the match server listens), the client
 *                        travels to the local match server (console "open 127.0.0.1:7777"): the lobby's mode select
 *                        and PLAY button need input.
 *   FNTEST_SHOTS=1       engine screenshots at the window's size ("shot", into the sandbox's Saved\Screenshots)
 *                        every 400 ms for 9 s after each of the first three deaths the ragdoll module sees, and one
 *                        when the client reaches the match. (HighResShot made 2048x1152 files and 300 ms hitches.)
 *
 * With either set, the game is also kept from taking the foreground or the mouse: its imports of SetForegroundWindow,
 * SetActiveWindow, BringWindowToTop, SetFocus, ClipCursor and SetCursorPos do nothing, and ShowWindow / SetWindowPos
 * never activate (the window style alone did not stop it when the client loaded into the match).
 */

static char g_join_flag[MAX_PATH]; static int g_join_done, g_shots;
static int g_mt, g_mt_lobby_done;   /* FNMODS_TEST, below */
static DWORD g_shot_from, g_shot_until, g_shot_next; static void *g_shot_ctx; static int32_t g_shot_ctx_serial; static int g_shot_bursts;

/* ---- no focus, no mouse: the exe's user32 imports replaced */
static BOOL WINAPI nf_true_h(HWND h) { (void)h; return TRUE; }
static HWND WINAPI nf_active(HWND h) { (void)h; return GetActiveWindow(); }
static HWND WINAPI nf_focus(HWND h) { (void)h; return GetFocus(); }
static BOOL WINAPI nf_clip(const RECT *r) { (void)r; return TRUE; }
static BOOL WINAPI nf_cursor(int x, int y) { (void)x; (void)y; return TRUE; }
static BOOL WINAPI nf_show(HWND h, int cmd)
{
    if (cmd == SW_SHOWNORMAL || cmd == SW_SHOWMAXIMIZED || cmd == SW_RESTORE || cmd == SW_SHOWDEFAULT) cmd = SW_SHOWNOACTIVATE;
    else if (cmd == SW_SHOW) cmd = SW_SHOWNA;
    else if (cmd == SW_MINIMIZE) cmd = SW_SHOWMINNOACTIVE;
    return ShowWindow(h, cmd);
}
static BOOL WINAPI nf_pos(HWND h, HWND after, int x, int y, int cx, int cy, UINT fl) { return SetWindowPos(h, after, x, y, cx, cy, fl | SWP_NOACTIVATE); }
static void no_focus(void)
{
    static const struct { const char *name; void *fn; } rep[] = {
        { "SetForegroundWindow", (void *)nf_true_h }, { "BringWindowToTop", (void *)nf_true_h }, { "SetActiveWindow", (void *)nf_active },
        { "SetFocus", (void *)nf_focus }, { "ClipCursor", (void *)nf_clip }, { "SetCursorPos", (void *)nf_cursor },
        { "ShowWindow", (void *)nf_show }, { "SetWindowPos", (void *)nf_pos },
    };
    enum { NREP = sizeof rep / sizeof rep[0] };
    HMODULE u32 = GetModuleHandleA("user32.dll");
    if (!u32) return;
    void *real[NREP];
    for (int k = 0; k < NREP; k++) real[k] = (void *)GetProcAddress(u32, rep[k].name);
    u8 *base = (u8 *)GetModuleHandleW(NULL);
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    int done = 0;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); dir.VirtualAddress && d->Name; d++) {
        if (_stricmp((char *)(base + d->Name), "user32.dll")) continue;
        for (void **slot = (void **)(base + d->FirstThunk); *slot; slot++)
            for (int k = 0; k < NREP; k++) if (real[k] && *slot == real[k]) {
                DWORD old; if (!VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old)) break;
                *slot = rep[k].fn; VirtualProtect(slot, sizeof *slot, old, &old); done++; break;
            }
    }
    logf_("test: %d focus and mouse imports of the game replaced", done);
}

static void test_init(void)
{
    DWORD n = GetEnvironmentVariableA("FNTEST_JOIN", g_join_flag, sizeof g_join_flag);
    if (!n || n >= sizeof g_join_flag) g_join_flag[0] = 0;
    char e[8]; n = GetEnvironmentVariableA("FNTEST_SHOTS", e, sizeof e);
    g_shots = n && n < sizeof e && e[0] == '1';
    if (g_join_flag[0] || g_shots) no_focus();
    if (g_join_flag[0] || g_shots) logf_("test: join when %s exists, screenshots %s", g_join_flag[0] ? g_join_flag : "(never)", g_shots ? "on" : "off");
}

static void test_on_death(void *pawn, int ragdoll)
{
    if (!g_shots || g_shot_bursts >= 3) return;
    g_shot_bursts++;
    g_shot_ctx = pawn; g_shot_ctx_serial = serial_of(pawn);
    g_shot_from = g_shot_next = GetTickCount(); g_shot_until = g_shot_from + 9000;
    logf_("test: death %d (%s): screenshots for 9 s", g_shot_bursts, ragdoll ? "ragdoll" : "observed");
}

static void test_tick(void)
{
    static DWORD last_check, match_shot;
    DWORD now = GetTickCount();
    if (g_join_flag[0] && !g_join_done && (!g_mt || g_mt_lobby_done) && now - last_check >= 1000) {
        last_check = now;
        if (GetFileAttributesA(g_join_flag) != INVALID_FILE_ATTRIBUTES) {
            void *pc = player_controller();
            if (pc) { console(pc, L"open 127.0.0.1:7777"); g_join_done = 1; match_shot = now + 60000; logf_("test: join sent"); }
        }
    }
    if (g_shots && match_shot && now >= match_shot) {
        match_shot = 0;
        void *pc = player_controller();
        if (pc) console(pc, L"shot");
    }
    if (g_shot_until && now >= g_shot_next) {
        if (now > g_shot_until) { g_shot_until = 0; logf_("test: screenshots done"); return; }
        void *ctx = alive(g_shot_ctx, g_shot_ctx_serial) ? g_shot_ctx : player_controller();
        if (ctx) console(ctx, L"shot");
        g_shot_next = now + 400;
    }
}

/* ---------------------------------------------------------------- FNMODS_TEST=1: the mods, driven from inside the game
   The keys a player would press are posted to the game's own window (the message hook and the engine take them like
   typed keys); screenshots are the engine's ("shot showui": the frame with Slate on it, the console included).
   Lobby: menu open, values changed with the arrows and Enter, menu closed, console opened, a command typed and run.
   Match (once the pawn has been there 25 s): fly through the menu, Space held, slow motion from the lobby carried over.
   The join of FNTEST_JOIN waits until the lobby part is done. */
static int g_mt_match_done, g_mt_step;
static DWORD g_mt_next, g_mt_since;
static void mt_init(void)
{
    char e[8]; DWORD n = GetEnvironmentVariableA("FNMODS_TEST", e, sizeof e);
    g_mt = n && n < sizeof e && e[0] == '1';
    if (g_mt) { logf_("test: mods test on"); if (!g_join_flag[0] && !g_shots) no_focus(); }
}
static void mt_chars(const char *s) { for (; *s; s++) PostMessageA(g_game_hwnd, WM_CHAR, (WPARAM)(u8)*s, 1); }
static void mt_shot(void *pc, const char *what) { logf_("test: screenshot: %s", what); console(pc, L"shot showui"); }
static void mt_keys(const UINT *vk, int n) { for (int i = 0; i < n; i++) post_key(vk[i]); }
static void mt_tick(void)
{
    if (!g_mt || !g_game_hwnd) return;
    DWORD now = GetTickCount();
    void *pc = SA.pc;
    if (!pc || !alive(pc, SA.pc_serial) || now < g_mt_next) return;
    if (!g_mt_lobby_done) {
        if (!SA.standalone) { g_mt_lobby_done = 1; return; }
        if (!MN.hud) return;
        if (!g_mt_since) { g_mt_since = now; g_mt_next = now + 20000; return; }
        switch (g_mt_step++) {
        case 0: mt_shot(pc, "lobby"); g_mt_next = now + 1500; break;
        case 1: post_key(VK_F8); g_mt_next = now + 1500; break;
        case 2: mt_shot(pc, "lobby, menu open"); g_mt_next = now + 1500; break;
        case 3: { static const UINT k[] = { VK_DOWN, VK_DOWN, VK_DOWN, VK_RETURN, VK_UP, VK_UP, VK_UP, VK_RETURN, VK_RIGHT, VK_RIGHT, VK_DOWN, VK_DOWN, VK_RETURN };
                  mt_keys(k, sizeof k / sizeof k[0]); g_mt_next = now + 2500; break; }
        case 4: mt_shot(pc, "lobby, menu: fov 100, slow motion 0.5, post effects off"); g_mt_next = now + 1500; break;
        case 5: post_key(VK_F8); g_mt_next = now + 2000; break;
        case 6: mt_shot(pc, "lobby, menu closed, settings on"); g_mt_next = now + 1500; break;
        case 7: logf_("test: console open: %d", console_open()); g_mt_next = now + 1000; break;
        case 8: mt_chars("stat fps"); g_mt_next = now + 1000; break;
        case 9: mt_shot(pc, "lobby, console typing"); g_mt_next = now + 1500; break;
        case 10: console_open(); g_mt_next = now + 1000; break;
        case 11: mt_shot(pc, "lobby, console open"); g_mt_next = now + 1500; break;
        case 12: post_key(VK_RETURN); g_mt_next = now + 1500; break;
        case 13: post_key(VK_ESCAPE); g_mt_next = now + 1500; break;
        case 14: mt_shot(pc, "lobby, after stat fps"); g_mt_next = now + 1500; break;
        default: g_mt_lobby_done = 1; g_mt_step = 0; g_mt_since = 0; logf_("test: lobby part done (menu drawn %ld times)", g_menu_draws); break;
        }
        return;
    }
    if (g_mt_match_done || SA.standalone) return;
    void *pawn = SN.o_pawn >= 0 ? AT(pc, SN.o_pawn, void *) : NULL;
    if (!pawn) { g_mt_since = 0; return; }
    if (!g_mt_since) { g_mt_since = now; g_mt_next = now + 25000; logf_("test: pawn %p in the match: mods test in 25 s", pawn); return; }
    switch (g_mt_step++) {
    case 0: mt_shot(pc, "match"); g_mt_next = now + 1500; break;
    case 1: { static const UINT k[] = { VK_F8, VK_DOWN, VK_DOWN, VK_DOWN, VK_DOWN, VK_RETURN }; g_menu_sel = 0; mt_keys(k, sizeof k / sizeof k[0]); g_mt_next = now + 2000; break; }
    case 2: mt_shot(pc, "match, menu: fly on"); g_mt_next = now + 1000; break;
    case 3: post_key(VK_F8); PostMessageA(g_game_hwnd, WM_KEYDOWN, VK_SPACE, 1 | ((LPARAM)MapVirtualKeyA(VK_SPACE, MAPVK_VK_TO_VSC) << 16)); g_mt_next = now + 2500; break;
    case 4: mt_shot(pc, "match, flying up (Space held)"); g_mt_next = now + 1500; break;
    case 5: { LPARAM sc = (LPARAM)MapVirtualKeyA(VK_SPACE, MAPVK_VK_TO_VSC) << 16; PostMessageA(g_game_hwnd, WM_KEYUP, VK_SPACE, 1 | sc | (1u << 30) | (1u << 31)); g_mt_next = now + 1500; break; }
    case 6: mt_shot(pc, "match, flying"); g_mt_next = now + 1500; break;
    default: g_mt_match_done = 1; logf_("test: match part done (menu drawn %ld times)", g_menu_draws); break;
    }
}
