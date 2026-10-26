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
static int g_ct;   /* FNMODS_TEST=2: the cheats test (below) instead */
static void mt_init(void)
{
    char e[8]; DWORD n = GetEnvironmentVariableA("FNMODS_TEST", e, sizeof e);
    g_mt = n && n < sizeof e && e[0] == '1';
    g_ct = n && n < sizeof e && e[0] == '2';
    g_aim_trace = g_ct;
    if (g_mt || g_ct) { logf_("test: %s test on", g_ct ? "cheats" : "mods"); if (!g_join_flag[0] && !g_shots) no_focus(); }
}
static void mt_chars(const char *s) { for (; *s; s++) PostMessageA(g_game_hwnd, WM_CHAR, (WPARAM)(u8)*s, 1); }
static void mt_shot(void *pc, const char *what) { logf_("test: screenshot: %s", what); console(pc, L"shot showui"); }
static void mt_keys(const UINT *vk, int n) { for (int i = 0; i < n; i++) post_key(vk[i]); }
/* what the match's mods did, measured: the pawn's height (AActor::K2_GetActorLocation) and the world's time dilation
   as the client sees it (UGameplayStatics::GetGlobalTimeDilation: the server's WorldSettings value, replicated) */
static void mt_measure(void *pc, void *pawn, const char *what)
{
    static ufn loc, dil; static int o_loc = -1, o_dctx = -1, o_dret = -1, o_vel = -1, o_acc = -1, done;
    if (!done) {
        done = 1;
        o_vel = prop_off(find_class("/Script/Engine", "MovementComponent"), "Velocity");
        o_acc = prop_off(find_class("/Script/Engine", "CharacterMovementComponent"), "Acceleration");
        loc = fn_of(find_class("/Script/Engine", "Actor"), "K2_GetActorLocation"); o_loc = parm(loc, "ReturnValue");
        dil = fn_of(gps() ? AT(gps(), O_CLASS, void *) : NULL, "GetGlobalTimeDilation");
        o_dctx = parm(dil, "WorldContextObject"); o_dret = parm(dil, "ReturnValue");
    }
    float z = NAN, td = NAN;
    if (loc.fn && o_loc >= 0) { u8 p[256]; memset(p, 0, sizeof p); call(pawn, loc, p); z = ((float *)(p + o_loc))[2]; }
    if (dil.fn && o_dctx >= 0 && o_dret >= 0) { u8 p[256]; memset(p, 0, sizeof p); AT(p, o_dctx, void *) = pc; call(gps(), dil, p); td = AT(p, o_dret, float); }
    void *cmc = SN.o_cmc >= 0 ? AT(pawn, SN.o_cmc, void *) : NULL;
    float vz = cmc && o_vel >= 0 ? ((float *)((u8 *)cmc + o_vel))[2] : NAN, az = cmc && o_acc >= 0 ? ((float *)((u8 *)cmc + o_acc))[2] : NAN;
    logf_("test: %s: pawn height %.1f, velocity z %.1f, acceleration z %.1f, Space %d, time dilation %.2f, movement mode %d",
          what, z, vz, az, g_key_up, td, movement_mode(pawn));
}
static void ct_tick(void);
static void mt_tick(void)
{
    if (g_ct) { ct_tick(); return; }
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
        case 1: post_key(key_vk(g_cfg.menu_key)); g_mt_next = now + 1500; break;
        case 2: mt_shot(pc, "lobby, menu open"); g_mt_next = now + 1500; break;
        case 3: { static const UINT k[] = { VK_DOWN, VK_DOWN, VK_DOWN, VK_RETURN, VK_UP, VK_UP, VK_UP, VK_RETURN, VK_RIGHT, VK_RIGHT, VK_DOWN, VK_DOWN, VK_RETURN };
                  mt_keys(k, sizeof k / sizeof k[0]); g_mt_next = now + 2500; break; }
        case 4: mt_shot(pc, "lobby, menu: fov 100, slow motion 0.5, post effects off"); g_mt_next = now + 1500; break;
        case 5: post_key(key_vk(g_cfg.menu_key)); g_mt_next = now + 2000; break;
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
    case 0: mt_measure(pc, pawn, "match"); mt_shot(pc, "match"); g_mt_next = now + 1500; break;
    case 1: { UINT k[] = { key_vk(g_cfg.menu_key), VK_DOWN, VK_DOWN, VK_DOWN, VK_DOWN, VK_RETURN }; g_menu_sel = 0; mt_keys(k, sizeof k / sizeof k[0]); g_mt_next = now + 2000; break; }
    case 2: mt_measure(pc, pawn, "fly on"); mt_shot(pc, "match, menu: fly on"); g_mt_next = now + 1000; break;
    case 3: mt_measure(pc, pawn, "Space down"); post_key(key_vk(g_cfg.menu_key)); PostMessageA(g_game_hwnd, WM_KEYDOWN, VK_SPACE, 1 | ((LPARAM)MapVirtualKeyA(VK_SPACE, MAPVK_VK_TO_VSC) << 16)); g_mt_next = now + 2500; break;
    case 4: mt_measure(pc, pawn, "Space held 2.5 s"); mt_shot(pc, "match, flying up (Space held)"); g_mt_next = now + 1500; break;
    case 5: { LPARAM sc = (LPARAM)MapVirtualKeyA(VK_SPACE, MAPVK_VK_TO_VSC) << 16; PostMessageA(g_game_hwnd, WM_KEYUP, VK_SPACE, 1 | sc | (1u << 30) | (1u << 31)); g_mt_next = now + 250; break; }
    /* after Space is released the pawn must hold its height: measured every 250 ms for 1.5 s */
    case 6: case 7: case 8: case 9: case 10: {
        char w[32]; snprintf(w, sizeof w, "Space up %d ms", (g_mt_step - 6) * 250);
        mt_measure(pc, pawn, w); g_mt_next = now + 250; break;
    }
    case 11: mt_measure(pc, pawn, "Space up 1.5 s"); mt_shot(pc, "match, flying"); g_mt_next = now + 1500; break;
    default: g_mt_match_done = 1; logf_("test: match part done (menu drawn %ld times)", g_menu_draws); break;
    }
}

/* ---------------------------------------------------------------- FNMODS_TEST=2: the cheats, driven from inside a match
   (mods.json of the run: aimAssist, cheatKeys on). Once the pawn has been in the match 25 s: the F8 menu with the cheats,
   F9 spawns bots (counted: alive, another team, distance), the right button held with an enemy off the crosshair (the aim
   module logs when the crosshair reaches the head), F6 with the camera looking down (the pawn's feet against the trace's
   point), a map pin sent as the client's ServerAddMapMarker and F7 (the pawn's X Y against the pin's), god mode against a
   fall (health before and after, with god on and off), a random POI from the menu and F9 again for teammates. */
static float ct_feet_err(void *pawn, float *dxy, float *dz, v3 *feet)
{
    v3 a = actor_loc(pawn); float hh = pawn_half_height(pawn);
    v3 f = { a.x, a.y, a.z - (hh == hh ? hh : 0) };
    *dxy = sqrtf((f.x - CA.tp[0]) * (f.x - CA.tp[0]) + (f.y - CA.tp[1]) * (f.y - CA.tp[1])); *dz = f.z - CA.tp[2];
    if (feet) *feet = f;
    return hh;
}
/* the other players: alive, of another team, and the nearest one's distance */
static int ct_count(void *pc, void *me, int *team_alive, float *nearest)
{
    void **arr; int n = player_array(pc, &arr), en = 0, tm = 0; float best = 1e9f;
    int my_team = team_of(my_player_state(me)); v3 at = actor_loc(me);
    for (int i = 0; i < n; i++) {
        void *ps = arr[i], *pawn = ps ? AT(ps, CH.o_pspawn, void *) : NULL;
        if (!pawn || pawn == me || !pawn_alive(pawn)) continue;
        if (team_of(ps) == my_team) { tm++; continue; }
        en++;
        v3 l = actor_loc(pawn); float d = sqrtf((l.x - at.x) * (l.x - at.x) + (l.y - at.y) * (l.y - at.y) + (l.z - at.z) * (l.z - at.z));
        if (d < best) best = d;
    }
    if (team_alive) *team_alive = tm;
    if (nearest) *nearest = best / 100;
    return en;
}
/* the frame without Slate ("shot", not "shot showui"): a client that joined from the console keeps the lobby's mode
   select over the match. Without showui the HUD's canvas is left out too, so the shots of the F8 menu take showui */
static void ct_shot(void *pc, const char *what) { logf_("test: screenshot: %s", what); console(pc, L"shot"); }
static void ct_shot_ui(void *pc, const char *what) { logf_("test: screenshot: %s", what); console(pc, L"shot showui"); }
static void ct_rmb(int down) { PostMessageA(g_game_hwnd, down ? WM_RBUTTONDOWN : WM_RBUTTONUP, down ? MK_RBUTTON : 0, MAKELPARAM(320, 180)); }
static void ct_tick(void)
{
    static int step, en0, tm0, aim_trial, aim_saved, aim_tries; static DWORD next, since; static float h0, god_h;
    if (!g_game_hwnd || CH.state <= 0) return;
    DWORD now = GetTickCount();
    void *pc = SA.pc;
    if (!pc || !alive(pc, SA.pc_serial) || SA.standalone || now < next) return;
    void *me = AT(pc, SN.o_pawn, void *);
    if (!me) { since = 0; return; }
    if (!since) { since = now; next = now + 25000; logf_("test: pawn %p in the match: cheats test in 25 s", me); return; }
    float dxy, dz; v3 f; int tm; float nd;
    switch (step++) {
    case 0: en0 = ct_count(pc, me, &tm0, &nd); logf_("test: before: %d enemies alive (nearest %.1f m), %d teammates", en0, nd, tm0);
            ct_shot(pc, "match"); next = now + 1500; break;
    case 1: g_menu_sel = MI_AIM; post_key(key_vk(g_cfg.menu_key)); next = now + 1500; break;
    case 2: ct_shot_ui(pc, "match, F8 menu with the cheats"); next = now + 1500; break;
    case 3: post_key(key_vk(g_cfg.menu_key)); next = now + 1000; break;
    /* spawn bots with F9 */
    case 4: post_key(VK_F9); logf_("test: F9 (spawn %d %s)", g_cfg.spawn_n, g_cfg.spawn_team ? "team" : "enemy"); next = now + 6000; break;
    case 5: { int en = ct_count(pc, me, &tm, &nd);
              logf_("test: spawn: %d enemies alive now (%+d), %d teammates (%+d), nearest enemy %.1f m away", en, en - en0, tm, tm - tm0, nd);
              ct_shot(pc, "match, after F9: bots spawned"); next = now + 1500; break; }
    /* aim: the nearest visible enemy put 20 degrees right of the crosshair, then the right button held */
    case 6: { v3 cam; float rot[3]; if (!cam_view(pc, &cam, rot)) { next = now + 500; step--; break; }
              v3 fwd = rot_dir(rot); float e;
              void *t = aim_pick(pc, me, cam, fwd, 180, &e);
              if (!t && ++aim_tries < 8) { logf_("test: aim: no visible enemy yet (%d enemies in the cone)", g_aim_in_cone); step--; next = now + 1000; break; }
              if (!t) { logf_("test: aim: no visible enemy"); step = 9; break; }
              aim_tries = 0;
              logf_("test: aim: nearest visible enemy %.1f deg off the crosshair", e);
              v3 h = head_of(t), d = { h.x - cam.x, h.y - cam.y, h.z - cam.z };
              /* from the current rotation's own winding (pitch 357 and -3 are the same view, but the camera blends between
                 the numbers: a jump from one to the other spins it) */
              u8 p[256]; memset(p, 0, sizeof p); float *nr = (float *)(p + CH.sr_rot), *cr = (float *)((u8 *)pc + CH.o_crot);
              nr[0] = cr[0] + norm180(rot[0] - cr[0]) + norm180(atan2f(d.z, sqrtf(d.x * d.x + d.y * d.y)) / RAD - rot[0]);
              nr[1] = cr[1] + norm180(rot[1] - cr[1]) + norm180(atan2f(d.y, d.x) / RAD - 20 - rot[1]); nr[2] = cr[2] + norm180(rot[2] - cr[2]);
              call(pc, CH.setrot, p);
              logf_("test: aim: enemy %p put 20 deg off the crosshair (strength %d)", t, g_cfg.aim_strength); next = now + 1500; break; }
    case 7: ct_shot(pc, "match, enemy off the crosshair"); ct_rmb(1); logf_("test: right button down"); next = now + 2500; break;
    case 8: ct_shot(pc, "match, aim assist on the head (right button held)"); next = now + 800; break;
    case 9: ct_rmb(0); logf_("test: right button up"); next = now + 1000;
            if (!aim_trial) { aim_trial = 1; aim_saved = g_cfg.aim_strength; g_cfg.aim_strength = 10; step = 6; }   /* again, snapping */
            else g_cfg.aim_strength = aim_saved;
            break;
    /* teleport to the crosshair: the camera 8 degrees down */
    case 10: { v3 cam; float rot[3]; if (!cam_view(pc, &cam, rot)) { next = now + 500; step--; break; }
               u8 p[256]; memset(p, 0, sizeof p); float *cr = (float *)((u8 *)pc + CH.o_crot), *nr = (float *)(p + CH.sr_rot);
               nr[0] = cr[0] + norm180(-8 - cr[0]); nr[1] = cr[1] + norm180(rot[1] - cr[1]); nr[2] = cr[2] + norm180(rot[2] - cr[2]);
               call(pc, CH.setrot, p); next = now + 1500; break; }
    case 11: ct_shot(pc, "match, crosshair before F6"); post_key(VK_F6); next = now + 2000; break;
    case 12: if (CA.has_tp == 1) { ct_feet_err(me, &dxy, &dz, &f); logf_("test: teleport to the crosshair: feet at %.0f %.0f %.0f, %.1f cm from the point in X Y, %.1f cm in Z", f.x, f.y, f.z, dxy, dz); }
             else logf_("test: teleport to the crosshair: nothing sent");
             ct_shot(pc, "match, after F6"); next = now + 1500; break;
    /* the map pin: the client's own ServerAddMapMarker, 60 m east of the pawn, then F7 */
    case 13: { if (!CH.marker || !CH.addmarker.fn || CH.am_req < 0 || CH.o_wpos < 0) { logf_("test: map pin: no marker component or position"); step = 16; break; }
               v3 a = actor_loc(me); u8 p[512]; memset(p, 0, sizeof p);
               float *w = (float *)(p + CH.am_req + CH.o_wpos); w[0] = a.x + 6000; w[1] = a.y; w[2] = a.z;
               process_event_fn vpe = (process_event_fn)(AT(CH.marker, 0, void **))[PE_INDEX];   /* through its own table: as the game calls it */
               vpe(CH.marker, CH.addmarker.fn, p);
               logf_("test: map pin sent at %.0f %.0f %.0f (seen by the hook: %d)", w[0], w[1], w[2], CA.has_pin); next = now + 1500; break; }
    case 14: post_key(VK_F7); next = now + 2500; break;
    case 15: if (CA.has_tp == 2) { ct_feet_err(me, &dxy, &dz, &f); logf_("test: teleport to the map pin: feet at %.0f %.0f %.0f, %.1f cm from the pin in X Y (pin Z %.0f)", f.x, f.y, f.z, dxy, CA.tp[2]); }
             else logf_("test: teleport to the map pin: nothing sent");
             ct_shot(pc, "match, after F7 (map pin)"); next = now + 1500; break;
    /* god mode against a fall of 20 m (sent as a teleport 20 m above the pawn's feet), on then off */
    case 16: g_cfg.god = 1; next = now + 1500; break;
    case 17: case 20: { v3 a = actor_loc(me); float hh = pawn_half_height(me); h0 = pawn_health(me);
               wchar_t cmd[128]; swprintf(cmd, 128, L"tpto %.1f %.1f %.1f 0 0 1", a.x, a.y, a.z - hh + 2000); server_cheat(pc, cmd);
               logf_("test: god %s: health %.0f, can be damaged %d, up 20 m", g_cfg.god ? "on" : "off", h0, CH.can_damage.off >= 0 ? bool_get(me, CH.can_damage) : -1);
               next = now + 6000; break; }
    case 18: case 21: god_h = pawn_health(me); logf_("test: god %s: health after the fall %.0f (was %.0f)", g_cfg.god ? "on" : "off", god_h, h0);
             next = now + 500; break;
    case 19: g_cfg.god = 0; next = now + 1500; break;
    /* a random POI from the menu */
    case 22: { v3 a = actor_loc(me); CA.tp[0] = a.x; CA.tp[1] = a.y; CA.tp[2] = a.z;
               g_menu_sel = MI_POI; post_key(key_vk(g_cfg.menu_key)); next = now + 1200; break; }
    case 23: ct_shot_ui(pc, "match, menu: random POI"); post_key(VK_RETURN); next = now + 8000; break;   /* the place streams in */
    case 24: { v3 a = actor_loc(me); logf_("test: random POI: the pawn moved %.0f m, now at %.0f %.0f %.0f", sqrtf((a.x - CA.tp[0]) * (a.x - CA.tp[0]) + (a.y - CA.tp[1]) * (a.y - CA.tp[1])) / 100, a.x, a.y, a.z);
               ct_shot(pc, "match, at a random POI"); next = now + 1500; break; }
    /* teammates with F9 */
    case 25: en0 = ct_count(pc, me, &tm0, &nd); g_cfg.spawn_team = 1; post_key(VK_F9); logf_("test: F9 (spawn %d team)", g_cfg.spawn_n); next = now + 6000; break;
    case 26: { int en = ct_count(pc, me, &tm, &nd); g_cfg.spawn_team = 0;
               logf_("test: spawn: %d teammates alive now (%+d), %d enemies (%+d)", tm, tm - tm0, en, en - en0);
               ct_shot(pc, "match, after F9: teammates spawned"); next = now + 1500; break; }
    default: g_ct = 0; logf_("test: cheats test done (menu drawn %ld times)", g_menu_draws); break;
    }
}
