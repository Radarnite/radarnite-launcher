/*
 * fnmenu.c - the in-game menu of fnmods.dll on F8 (#included by fnmods.c): the settings of fnsettings.c, the cheats of
 * fncheats.c (their switches, and Enter runs the teleports and the bot spawn) and the console, changed while playing. Changes last for the session; mods.json stays the launcher's.
 *
 * Drawn by the engine, on the canvas of the player's HUD: UGameViewportClient::Draw calls AHUD::PostRender on every
 * local player's HUD, AHUD::DrawHUD calls the blueprint event ReceiveDrawHUD through ProcessEvent, and during that
 * event AHUD::DrawRect / DrawText / GetTextSize (UFunctions) draw on the HUD's canvas. The HUD object gets its own copy
 * of its class's virtual table whose ProcessEvent entry runs the original and then, on ReceiveDrawHUD, draws the menu
 * (the same per-object table copy as fnragdoll.c: an object's vtable pointer is data, no game code changes).
 * It draws on the HUD's DebugCanvas, not its Canvas: AHUD::Canvas is under the game's UMG widgets (the lobby's screens
 * cover it all), DebugCanvas is "the 'foreground' debug canvas, will draw in front of Slate UI" (UE 4.26 HUD.h), the
 * one the console draws on. AHUD::DrawRect / DrawText draw on AHUD::Canvas, so for the menu's calls the HUD's Canvas
 * property holds the debug canvas, and gets its own back right after (both are UPROPERTYs: data, set during
 * PostRender by UGameViewportClient::Draw through AHUD::SetCanvas). Being part of the viewport's frame it shows in
 * fullscreen and in the engine's screenshots. While the menu is open the HUD's bShowHUD is kept on (put back as it
 * was when it closes).
 * Keys: the WM_KEYDOWN / WM_KEYUP messages of the game window, seen in fnmods.c's message hook before the game gets
 * them; F8 toggles the menu, and while it is open the arrows, Enter, Escape and Backspace are the menu's (taken from
 * the game). Nothing else is taken.
 */

/* the first five keep their places: the mods test (fntest.c) reaches them with the arrows */
enum { MI_FOV, MI_FPS, MI_FX, MI_SLOMO, MI_FLY, MI_AIM, MI_CONE, MI_GOD, MI_KEYS, MI_TPX, MI_TPPIN, MI_POI, MI_SPAWN, MI_CONSOLE, MI_N };
static const int FPS_STEPS[] = { 30, 60, 120, 144, 165, 240, 0 };
static const float SLOMO_STEPS[] = { 0.1f, 0.25f, 0.5f, 0.75f };
static const int CONE_STEPS[] = { 5, 10, 15, 20, 30, 45, 60, 90, 180 };
static const int SPAWN_STEPS[] = { 1, 2, 3, 5, 10, 20 };

static struct {
    int state;
    void *hud_cls; int o_myhud, o_canvas, o_dcanvas, o_csx, o_csy; boolp show;
    void *f_draw; int d_sx, d_sy;
    ufn rect, text, tsize;
    int r_col, r_x, r_y, r_w, r_h, t_text, t_col, t_x, t_y, t_font, t_scale, z_text, z_w, z_h, z_font, z_scale;
    void *hud; int32_t hud_serial; int show_was, show_forced;
    int dead;
} MN;
static volatile int g_menu_open, g_menu_sel;
static volatile LONG g_menu_draws;

static const need MENU_NEEDS[] = {
    { 'c', "HUD" }, { 'p', "PlayerController.MyHUD" }, { 'f', "HUD.ReceiveDrawHUD" }, { 'f', "HUD.DrawRect" }, { 'f', "HUD.DrawText" },
    { 'f', "HUD.GetTextSize" }, { 'p', "HUD.bShowHUD" }, { 'p', "HUD.Canvas", 1 }, { 'p', "HUD.DebugCanvas", 1 },
};

static int menu_resolve(void)
{
    MN.hud_cls = find_class("/Script/Engine", "HUD");
    void *pcc = find_class("/Script/Engine", "PlayerController");
    MN.o_myhud = pcc ? prop_off(pcc, "MyHUD") : -1;
    MN.show = bool_prop(MN.hud_cls, "bShowHUD");
    MN.o_canvas = MN.hud_cls ? prop_off(MN.hud_cls, "Canvas") : -1; MN.o_dcanvas = MN.hud_cls ? prop_off(MN.hud_cls, "DebugCanvas") : -1;
    void *ccls = find_class("/Script/Engine", "Canvas");
    MN.o_csx = ccls ? prop_off(ccls, "SizeX") : -1; MN.o_csy = ccls ? prop_off(ccls, "SizeY") : -1;
    logf_("menu: HUD.Canvas %d, HUD.DebugCanvas %d, Canvas.SizeX %d, SizeY %d%s", MN.o_canvas, MN.o_dcanvas, MN.o_csx, MN.o_csy,
          MN.o_canvas >= 0 && MN.o_dcanvas >= 0 ? "" : ": the menu draws under the game's UI");
    MN.f_draw = MN.hud_cls ? find_func(MN.hud_cls, "ReceiveDrawHUD") : NULL;
    MN.d_sx = MN.f_draw ? prop_off(MN.f_draw, "SizeX") : -1; MN.d_sy = MN.f_draw ? prop_off(MN.f_draw, "SizeY") : -1;
    MN.rect = fn_of(MN.hud_cls, "DrawRect"); MN.text = fn_of(MN.hud_cls, "DrawText"); MN.tsize = fn_of(MN.hud_cls, "GetTextSize");
    MN.r_col = parm(MN.rect, "RectColor"); MN.r_x = parm(MN.rect, "ScreenX"); MN.r_y = parm(MN.rect, "ScreenY"); MN.r_w = parm(MN.rect, "ScreenW"); MN.r_h = parm(MN.rect, "ScreenH");
    MN.t_text = parm(MN.text, "Text"); MN.t_col = parm(MN.text, "TextColor"); MN.t_x = parm(MN.text, "ScreenX"); MN.t_y = parm(MN.text, "ScreenY");
    MN.t_font = parm(MN.text, "Font"); MN.t_scale = parm(MN.text, "Scale");
    MN.z_text = parm(MN.tsize, "Text"); MN.z_w = parm(MN.tsize, "OutWidth"); MN.z_h = parm(MN.tsize, "OutHeight"); MN.z_font = parm(MN.tsize, "Font"); MN.z_scale = parm(MN.tsize, "Scale");
    logf_("menu: HUD %p (MyHUD %d, bShowHUD %d/0x%x), ReceiveDrawHUD %p (%d %d), DrawRect %p (%d %d %d %d %d), DrawText %p (%d %d %d %d %d %d), GetTextSize %p (%d %d %d %d %d)",
          MN.hud_cls, MN.o_myhud, MN.show.off, MN.show.mask, MN.f_draw, MN.d_sx, MN.d_sy, MN.rect.fn, MN.r_col, MN.r_x, MN.r_y, MN.r_w, MN.r_h,
          MN.text.fn, MN.t_text, MN.t_col, MN.t_x, MN.t_y, MN.t_font, MN.t_scale, MN.tsize.fn, MN.z_text, MN.z_w, MN.z_h, MN.z_font, MN.z_scale);
    int ok = MN.hud_cls && MN.o_myhud >= 0 && MN.f_draw && MN.d_sx >= 0 && MN.d_sy >= 0 && MN.rect.fn && MN.text.fn && MN.r_col >= 0 && MN.r_x >= 0 &&
             MN.r_y >= 0 && MN.r_w >= 0 && MN.r_h >= 0 && MN.t_text >= 0 && MN.t_col >= 0 && MN.t_x >= 0 && MN.t_y >= 0;
    return ok ? 1 : -1;
}

/* ---- drawing (inside ReceiveDrawHUD, on the game thread) */
typedef struct { float r, g, b, a; } lcolor;
static void *g_draw_hud; static int g_draw_w, g_draw_h;
static void m_rect(float x, float y, float w, float h, lcolor c)
{
    u8 p[512]; memset(p, 0, sizeof p);
    *(lcolor *)(p + MN.r_col) = c; AT(p, MN.r_x, float) = x; AT(p, MN.r_y, float) = y; AT(p, MN.r_w, float) = w; AT(p, MN.r_h, float) = h;
    call(g_draw_hud, MN.rect, p);
}
static void m_text(const wchar_t *s, float x, float y, float scale, lcolor c)
{
    u8 p[512]; memset(p, 0, sizeof p);
    put_fstring(p, MN.t_text, s);
    *(lcolor *)(p + MN.t_col) = c; AT(p, MN.t_x, float) = x; AT(p, MN.t_y, float) = y;
    if (MN.t_scale >= 0) AT(p, MN.t_scale, float) = scale;
    call(g_draw_hud, MN.text, p);
}
static float m_width(const wchar_t *s, float scale, float *h)
{
    if (!MN.tsize.fn || MN.z_text < 0 || MN.z_w < 0) { if (h) *h = 0; return 0; }
    u8 p[512]; memset(p, 0, sizeof p);
    put_fstring(p, MN.z_text, s);
    if (MN.z_scale >= 0) AT(p, MN.z_scale, float) = scale;
    call(g_draw_hud, MN.tsize, p);
    if (h) *h = MN.z_h >= 0 ? AT(p, MN.z_h, float) : 0;
    return AT(p, MN.z_w, float);
}
static int menu_in_match(void) { return SA.pc && !SA.standalone; }
/* the value column of an item; on = the item's switch */
static void menu_value(int i, wchar_t *v, int cap, int *on, int *dim)
{
    *dim = 0;
    switch (i) {
    case MI_FOV: *on = g_cfg.fov_on; swprintf(v, cap, L"%d°", (int)g_cfg.fov); break;
    case MI_FPS: *on = g_cfg.fps_on; if (g_cfg.fps) swprintf(v, cap, L"%d fps", g_cfg.fps); else swprintf(v, cap, L"no cap"); break;
    case MI_FX: *on = g_cfg.nofx; v[0] = 0; break;
    case MI_SLOMO: *on = g_cfg.slomo_on; swprintf(v, cap, L"%gx", g_cfg.slomo); break;
    case MI_FLY: *on = g_cfg.fly_on; swprintf(v, cap, L"%ls", g_cfg.ghost ? L"through walls" : L"fly"); *dim = !menu_in_match(); break;
    case MI_AIM: *on = g_cfg.aim_on; swprintf(v, cap, g_cfg.aim_strength >= 10 ? L"snap" : L"strength %d", g_cfg.aim_strength); *dim = !menu_in_match(); break;
    case MI_CONE: *on = -1; swprintf(v, cap, L"%d°", g_cfg.aim_cone); *dim = !menu_in_match(); break;
    case MI_GOD: *on = g_cfg.god; v[0] = 0; *dim = !menu_in_match(); break;
    case MI_KEYS: *on = g_cfg.keys; swprintf(v, cap, L"F6 F7 F9"); *dim = !menu_in_match(); break;
    case MI_TPX: *on = -1; swprintf(v, cap, g_cfg.keys ? L"F6" : L"Enter"); *dim = !menu_in_match(); break;
    case MI_TPPIN: *on = -1; swprintf(v, cap, !CA.has_pin ? L"no pin yet" : g_cfg.keys ? L"F7" : L"Enter"); *dim = !menu_in_match() || !CA.has_pin; break;
    case MI_POI: *on = -1; swprintf(v, cap, L"Enter"); *dim = !menu_in_match(); break;
    case MI_SPAWN: *on = -1; swprintf(v, cap, L"%d %ls%ls", g_cfg.spawn_n, g_cfg.spawn_team ? L"teammate" : L"enem",
                                      g_cfg.spawn_team ? (g_cfg.spawn_n == 1 ? L"" : L"s") : (g_cfg.spawn_n == 1 ? L"y" : L"ies"));
                   *dim = !menu_in_match(); break;
    default: *on = -1; swprintf(v, cap, L"%hs", CN.key_now[0] ? CN.key_now : "-"); *dim = !CN.made; break;
    }
}
static const wchar_t *MENU_LABEL[MI_N] = { L"Field of view", L"FPS cap", L"Remove post effects", L"Slow motion", L"Fly",
    L"Aim assist (hold right mouse)", L"Aim cone", L"God mode", L"Cheat keys", L"Teleport to the crosshair", L"Teleport to the map pin",
    L"Teleport to a random POI", L"Spawn bots", L"Open the console" };
static void menu_draw(void)
{
    float W = (float)g_draw_w, H = (float)g_draw_h;
    if (W < 64 || H < 64) return;
    float k = H / 1080.0f;                       /* the layout is made for 1080 lines and scaled */
    float ts = 1.0f, th = 0;
    m_width(L"Ag", ts, &th);
    if (th <= 0) th = 24;
    ts = 26.0f * k / th; if (ts < 0.35f) ts = 0.35f;   /* text 26 px at 1080 lines whatever the engine's font size is */
    float lh = 46 * k, pad = 22 * k, w = 660 * k, x = 48 * k, y = 110 * k;
    float h = pad * 2 + lh * (MI_N + 1) + 34 * k;
    lcolor bg = { 0.02f, 0.025f, 0.06f, 1.0f }, sel = { 0.45f, 0.33f, 1.0f, 0.55f }, white = { 1, 1, 1, 1 }, mute = { 1, 1, 1, 0.55f },
           green = { 0.25f, 0.85f, 0.45f, 1 }, off = { 1, 1, 1, 0.35f }, accent = { 0.72f, 0.61f, 1.0f, 1 };
    m_rect(x, y, w, h, bg);
    m_rect(x, y, w, 3 * k, accent);
    float cy = y + pad;
    m_text(L"RADARNITE MODS", x + pad, cy + (lh - 26 * k) / 2, ts * 0.95f, accent);
    float hw = m_width(L"F8 to close", ts * 0.8f, NULL);
    m_text(L"F8 to close", x + w - pad - hw, cy + (lh - 22 * k) / 2, ts * 0.8f, mute);
    cy += lh;
    for (int i = 0; i < MI_N; i++) {
        if (i == g_menu_sel) m_rect(x + 8 * k, cy, w - 16 * k, lh, sel);
        wchar_t v[48]; int on, dim; menu_value(i, v, 48, &on, &dim);
        float ty = cy + (lh - 26 * k) / 2;
        m_text(MENU_LABEL[i], x + pad, ty, ts, dim ? off : white);
        float vx = x + w - pad;
        if (on >= 0) {
            const wchar_t *sw = on ? L"ON" : L"OFF";
            float sw_w = m_width(sw, ts * 0.85f, NULL);
            vx -= sw_w; m_text(sw, vx, ty + 2 * k, ts * 0.85f, dim ? off : on ? green : off);
            vx -= 18 * k;
        }
        if (v[0]) { float vw = m_width(v, ts, NULL); m_text(v, vx - vw, ty, ts, dim ? off : on == 0 ? mute : white); }
        cy += lh;
    }
    const wchar_t *hint = g_menu_sel >= MI_FLY && g_menu_sel < MI_CONSOLE && !menu_in_match() ? L"This works in a match" :
                          g_menu_sel >= MI_TPX && g_menu_sel <= MI_SPAWN ? L"Enter: do it now   ←→ change" :
                          L"↑↓ choose   ←→ change   Enter on/off";
    m_text(hint, x + pad, cy + 6 * k, ts * 0.75f, mute);
    InterlockedIncrement(&g_menu_draws);
}

/* ---- the HUD's ProcessEvent */
typedef struct { void **orig, **clone; process_event_fn pe; } mvt;
static mvt g_mvt[4]; static int g_nmvt;
static void menu_pe_hook(void *obj, void *fn, void *parms)
{
    void **vt = *(void ***)obj;
    process_event_fn pe = g_pe;
    for (int k = 0; k < g_nmvt; k++) if (g_mvt[k].clone == vt) { pe = g_mvt[k].pe; break; }
    pe(obj, fn, parms);
    if (fn != MN.f_draw || !g_menu_open || MN.dead || GetCurrentThreadId() != g_game_tid) return;
    g_draw_hud = obj; g_draw_w = AT(parms, MN.d_sx, int32_t); g_draw_h = AT(parms, MN.d_sy, int32_t);
    void *canvas = MN.o_canvas >= 0 ? AT(obj, MN.o_canvas, void *) : NULL, *dbg = MN.o_dcanvas >= 0 ? AT(obj, MN.o_dcanvas, void *) : NULL;
    if (canvas && dbg) {
        AT(obj, MN.o_canvas, void *) = dbg;
        if (MN.o_csx >= 0 && MN.o_csy >= 0 && AT(dbg, MN.o_csx, int32_t) > 0 && AT(dbg, MN.o_csy, int32_t) > 0) {
            g_draw_w = AT(dbg, MN.o_csx, int32_t); g_draw_h = AT(dbg, MN.o_csy, int32_t);
        }
    }
    int fault = guarded(menu_draw);
    if (canvas && dbg) AT(obj, MN.o_canvas, void *) = canvas;
    if (fault) { MN.dead = 1; g_menu_open = 0; logf_("menu: FAULT while drawing: menu off for this session"); }
    static int said;
    if (!said) { said = 1; logf_("menu: first draw on %s (%d x %d)", canvas && dbg ? "the debug canvas" : "the HUD canvas", g_draw_w, g_draw_h); }
}
static int menu_swap(void *o)
{
    void **vt = AT(o, 0, void **);
    for (int k = 0; k < g_nmvt; k++) if (g_mvt[k].clone == vt) return 1;
    mvt *v = NULL;
    for (int k = 0; k < g_nmvt; k++) if (g_mvt[k].orig == vt) v = &g_mvt[k];
    if (!v) {
        if (g_nmvt >= (int)(sizeof g_mvt / sizeof g_mvt[0])) return 0;
        void *e = vt[PE_INDEX];
        if (e != (void *)g_pe && e != actor_pe()) { logf_("menu: vtable %p: entry 0x%x %p is not ProcessEvent, left alone", vt, PE_INDEX, e); return 0; }
        void **mem = VirtualAlloc(NULL, (VT_SLOTS + 1) * sizeof(void *), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!mem) return 0;
        int n = 0;
        while (n < VT_SLOTS + 1 && rd(vt - 1 + n, &mem[n], sizeof(void *))) n++;
        if (n < PE_INDEX + 2) { VirtualFree(mem, 0, MEM_RELEASE); return 0; }
        mem[1 + PE_INDEX] = (void *)menu_pe_hook;
        v = &g_mvt[g_nmvt++]; v->orig = vt; v->clone = mem + 1; v->pe = (process_event_fn)e;
        logf_("menu: HUD vtable %p copied (%d entries), ProcessEvent watched for ReceiveDrawHUD", vt, n - 1);
    }
    AT(o, 0, void **) = v->clone;
    return 1;
}

/* ---- keys (message hook, game thread): 1 = the game does not get it */
static void menu_step(int i, int dir)
{
    switch (i) {
    case MI_FOV: g_cfg.fov = clampf(g_cfg.fov + 5 * dir, 60, 120); g_cfg.fov_on = 1; break;
    case MI_FPS: {
        int n = (int)(sizeof FPS_STEPS / sizeof FPS_STEPS[0]), k = 0;
        for (int j = 0; j < n; j++) if (FPS_STEPS[j] == g_cfg.fps) k = j;
        k = (k + dir + n) % n; g_cfg.fps = FPS_STEPS[k]; g_cfg.fps_on = 1; break;
    }
    case MI_SLOMO: {
        int n = (int)(sizeof SLOMO_STEPS / sizeof SLOMO_STEPS[0]), k = 2;
        for (int j = 0; j < n; j++) if (fabsf(SLOMO_STEPS[j] - g_cfg.slomo) < 0.01f) k = j;
        k += dir; if (k < 0) k = 0; if (k >= n) k = n - 1; g_cfg.slomo = SLOMO_STEPS[k]; g_cfg.slomo_on = 1; break;
    }
    case MI_FLY: g_cfg.ghost = !g_cfg.ghost; break;
    case MI_AIM: g_cfg.aim_strength = g_cfg.aim_strength + dir < 1 ? 1 : g_cfg.aim_strength + dir > 10 ? 10 : g_cfg.aim_strength + dir; break;
    case MI_CONE: {
        int n = (int)(sizeof CONE_STEPS / sizeof CONE_STEPS[0]), k = 4;
        for (int j = 0; j < n; j++) if (CONE_STEPS[j] == g_cfg.aim_cone) k = j;
        k += dir; if (k < 0) k = 0; if (k >= n) k = n - 1; g_cfg.aim_cone = CONE_STEPS[k]; break;
    }
    case MI_SPAWN: {   /* 1 ... 20 enemies, then 1 ... 20 teammates */
        int n = (int)(sizeof SPAWN_STEPS / sizeof SPAWN_STEPS[0]), k = 2;
        for (int j = 0; j < n; j++) if (SPAWN_STEPS[j] == g_cfg.spawn_n) k = j;
        k += g_cfg.spawn_team ? n : 0; k = (k + dir + 2 * n) % (2 * n);
        g_cfg.spawn_team = k >= n; g_cfg.spawn_n = SPAWN_STEPS[k % n]; break;
    }
    default: break;
    }
}
static void menu_toggle(int i)
{
    switch (i) {
    case MI_FOV: g_cfg.fov_on = !g_cfg.fov_on; break;
    case MI_FPS: g_cfg.fps_on = !g_cfg.fps_on; break;
    case MI_FX: g_cfg.nofx = !g_cfg.nofx; break;
    case MI_SLOMO: g_cfg.slomo_on = !g_cfg.slomo_on; break;
    case MI_FLY: g_cfg.fly_on = !g_cfg.fly_on; break;
    case MI_AIM: g_cfg.aim_on = !g_cfg.aim_on; break;
    case MI_GOD: g_cfg.god = !g_cfg.god; break;
    case MI_KEYS: g_cfg.keys = !g_cfg.keys; break;
    case MI_TPX: if (menu_in_match()) { g_menu_open = 0; g_ch_action = 1; } break;
    case MI_TPPIN: if (menu_in_match()) { g_menu_open = 0; g_ch_action = 2; } break;
    case MI_POI: if (menu_in_match()) { g_menu_open = 0; g_ch_action = 4; } break;
    case MI_SPAWN: if (menu_in_match()) g_ch_action = 3; break;
    case MI_CONSOLE: g_menu_open = 0; console_open(); break;
    }
}
static void menu_set_open(int open)
{
    g_menu_open = open;
    logf_("menu: %s", open ? "open" : "closed");
}
static int menu_key(UINT msg, WPARAM vk)
{
    if (!g_cfg.menu || MN.state <= 0 || MN.dead) return 0;
    int down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    if (vk == VK_F8) { if (down) menu_set_open(!g_menu_open); return 1; }
    if (!g_menu_open) return 0;
    switch (vk) {
    case VK_UP: if (down) g_menu_sel = (g_menu_sel + MI_N - 1) % MI_N; return 1;
    case VK_DOWN: if (down) g_menu_sel = (g_menu_sel + 1) % MI_N; return 1;
    case VK_LEFT: if (down) menu_step(g_menu_sel, -1); return 1;
    case VK_RIGHT: if (down) menu_step(g_menu_sel, 1); return 1;
    case VK_RETURN: if (down) menu_toggle(g_menu_sel); return 1;
    case VK_ESCAPE: case VK_BACK: if (down) menu_set_open(0); return 1;
    }
    return 0;
}

static void menu_tick(void)
{
    static DWORD next;
    DWORD now = GetTickCount();
    if (!g_cfg.menu && g_menu_open) menu_set_open(0);
    /* bShowHUD on while open, as it was when closed */
    if (MN.hud && alive(MN.hud, MN.hud_serial) && MN.show.off >= 0) {
        if (g_menu_open && !MN.show_forced && !bool_get(MN.hud, MN.show)) { MN.show_forced = 1; bool_set(MN.hud, MN.show, 1); logf_("menu: bShowHUD turned on while the menu is open"); }
        if (!g_menu_open && MN.show_forced) { MN.show_forced = 0; bool_set(MN.hud, MN.show, 0); }
    }
    if (now < next) return;
    next = now + 500;
    void *pc = local_pc();
    if (!pc) return;
    if (MN.state == 0) { MN.state = menu_resolve(); if (MN.state < 0) logf_("menu: FAIL: engine types not found: no in-game menu"); }
    if (MN.state < 0 || MN.dead) return;
    void *hud = AT(pc, MN.o_myhud, void *);
    if (!hud || (hud == MN.hud && alive(MN.hud, MN.hud_serial))) return;
    MN.hud = hud; MN.hud_serial = serial_of(hud); MN.show_forced = 0;
    char a[96];
    if (menu_swap(hud)) logf_("menu: HUD %p (%s) ready, bShowHUD %d", hud, obj_name(AT(hud, O_CLASS, void *), a, sizeof a), bool_get(hud, MN.show));
}
