/*
 * fnconfig.c - mods.json, the launcher's mod settings, for fnmods.dll (#included by fnmods.c).
 *
 * The launcher writes it (Mods tab) and passes its path in FNMODS_CONFIG; every mod is an object with "on" and, for
 * some, "value":
 *   { "version": 1,
 *     "bodySliders": {"on": true}, "ragdoll": {"on": true},
 *     "console": {"on": true, "value": "F10"},      engine console; value = its key (an engine key name)
 *     "menu": {"on": true, "value": "F8"},           the in-game menu; value = its key (F8 by default; names: fnkeys.c)
 *     "fov": {"on": false, "value": 90},            field of view, degrees (60-120)
 *     "fpsCap": {"on": false, "value": 60},         frame rate cap (0 = none)
 *     "noPostEffects": {"on": false},               motion blur, depth of field, bloom, lens flares, fringe, grain off
 *     "slowMotion": {"on": false, "value": 0.5},    time dilation (0.1-1); in a match the match server applies it
 *     "fly": {"on": false, "value": "fly"},        "fly" or "ghost" (also through walls); match only
 *     "aimAssist": {"on": false, "value": 5},       hold the right mouse button: the camera turns to the nearest visible enemy's
 *                                                   head; value = strength 1-10: turns at most 60 degrees a second per step, 10 snaps
 *     "aimCone": {"value": 30},                     only enemies at most this many degrees off the crosshair (5-180)
 *     "god": {"on": false},                         the player's pawn takes no damage (match only)
 *     "cheatKeys": {"on": false},                   F6 teleport to the crosshair, F7 to the map pin, F9 spawn bots (match only)
 *     "spawnBots": {"value": 3, "side": "enemy"} }  how many bots F9 and the menu spawn (1-20), "enemy" or "team"
 * A missing file or key keeps the default below. The worker thread reads the file again when it changes (the launcher
 * saved it while the game runs); the game thread takes the new copy at its next tick. The in-game menu changes the
 * game thread's copy for the session only: the file stays the launcher's.
 */

typedef struct {
    int body, ragdoll, console, menu;
    char console_key[24], menu_key[24];
    int fov_on; float fov;
    int fps_on; int fps;
    int nofx;
    int slomo_on; float slomo;
    int fly_on; int ghost;
    int aim_on, aim_strength, aim_cone;
    int god, keys;
    int spawn_n, spawn_team;
} mods_cfg;
static mods_cfg g_cfg;                     /* the game thread's copy: what the modules follow */

static void cfg_defaults(mods_cfg *c)
{
    memset(c, 0, sizeof *c);
    c->body = c->ragdoll = c->console = c->menu = 1;
    strcpy(c->console_key, "F10"); strcpy(c->menu_key, "F8");
    c->fov = 90; c->fps = 60; c->slomo = 0.5f;
    c->aim_strength = 5; c->aim_cone = 30; c->spawn_n = 3;
}

/* ---- a small JSON reader: enough for this file (objects, strings, numbers, true/false/null, arrays skipped) */
typedef struct { const char *p, *end; int bad; } jcur;
static void j_ws(jcur *j) { while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\r' || *j->p == '\n')) j->p++; }
static int j_str(jcur *j, char *out, int cap)
{
    j_ws(j);
    if (j->p >= j->end || *j->p != '"') { j->bad = 1; return 0; }
    j->p++; int n = 0;
    while (j->p < j->end && *j->p != '"') {
        char ch = *j->p++;
        if (ch == '\\' && j->p < j->end) { ch = *j->p++; if (ch == 'u') { j->p += 4; ch = '?'; } else if (ch == 'n') ch = '\n'; else if (ch == 't') ch = '\t'; }
        if (n < cap - 1) out[n++] = ch;
    }
    if (j->p >= j->end) { j->bad = 1; return 0; }
    j->p++; out[n] = 0;
    return 1;
}
static void j_skip(jcur *j);
/* a value: kind 's' string (into s), 'n' number (into *num), 'b' bool (into *num), 'o' object / 'a' array (skipped
   unless the caller reads it), 'z' null */
static char j_peek(jcur *j)
{
    j_ws(j);
    if (j->p >= j->end) { j->bad = 1; return 0; }
    char ch = *j->p;
    if (ch == '{') return 'o';
    if (ch == '[') return 'a';
    if (ch == '"') return 's';
    if (ch == 't' || ch == 'f') return 'b';
    if (ch == 'n') return 'z';
    return 'n';
}
static double j_num(jcur *j)
{
    char b[48]; int n = 0;
    j_ws(j);
    while (j->p < j->end && n < 47 && strchr("+-0123456789.eE", *j->p)) b[n++] = *j->p++;
    b[n] = 0;
    if (!n) j->bad = 1;
    return atof(b);
}
static int j_bool(jcur *j)
{
    j_ws(j);
    if (j->end - j->p >= 4 && !memcmp(j->p, "true", 4)) { j->p += 4; return 1; }
    if (j->end - j->p >= 5 && !memcmp(j->p, "false", 5)) { j->p += 5; return 0; }
    j->bad = 1; return 0;
}
static void j_skip(jcur *j)
{
    char k = j_peek(j); char tmp[256];
    if (j->bad) return;
    if (k == 's') { j_str(j, tmp, sizeof tmp); return; }
    if (k == 'n') { j_num(j); return; }
    if (k == 'b') { j_bool(j); return; }
    if (k == 'z') { j->p += j->end - j->p >= 4 ? 4 : j->end - j->p; return; }
    char close = k == 'o' ? '}' : ']';
    j->p++;
    for (int first = 1; ; first = 0) {
        j_ws(j);
        if (j->p < j->end && *j->p == close) { j->p++; return; }
        if (!first) { if (j->p >= j->end || *j->p != ',') { j->bad = 1; return; } j->p++; }
        if (k == 'o') { j_str(j, tmp, sizeof tmp); j_ws(j); if (j->p >= j->end || *j->p != ':') { j->bad = 1; return; } j->p++; }
        j_skip(j);
        if (j->bad) return;
    }
}
/* a mod's object: "on", "value" (number or string) and, for spawnBots, "side" */
typedef struct { int has_on, on, has_num, has_str; double num; char str[32]; char side[16]; } jmod;
static void j_mod(jcur *j, jmod *m)
{
    memset(m, 0, sizeof *m);
    if (j_peek(j) != 'o') { j_skip(j); return; }
    j->p++;
    for (int first = 1; ; first = 0) {
        j_ws(j);
        if (j->p < j->end && *j->p == '}') { j->p++; return; }
        if (!first) { if (j->p >= j->end || *j->p != ',') { j->bad = 1; return; } j->p++; }
        char key[32]; if (!j_str(j, key, sizeof key)) return;
        j_ws(j); if (j->p >= j->end || *j->p != ':') { j->bad = 1; return; } j->p++;
        char k = j_peek(j);
        if (!strcmp(key, "on") && k == 'b') { m->has_on = 1; m->on = j_bool(j); }
        else if (!strcmp(key, "value") && k == 'n') { m->has_num = 1; m->num = j_num(j); }
        else if (!strcmp(key, "value") && k == 's') { m->has_str = 1; j_str(j, m->str, sizeof m->str); }
        else if (!strcmp(key, "side") && k == 's') j_str(j, m->side, sizeof m->side);
        else j_skip(j);
        if (j->bad) return;
    }
}
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
/* parse text over the defaults in c: 1 = read (unknown keys ignored), 0 = not JSON (c keeps the defaults) */
static int cfg_parse(const char *text, size_t len, mods_cfg *c)
{
    jcur j = { text, text + len, 0 };
    if (len >= 3 && (u8)text[0] == 0xEF && (u8)text[1] == 0xBB && (u8)text[2] == 0xBF) j.p += 3;
    if (j_peek(&j) != 'o') return 0;
    j.p++;
    for (int first = 1; ; first = 0) {
        j_ws(&j);
        if (j.p < j.end && *j.p == '}') return 1;
        if (!first) { if (j.p >= j.end || *j.p != ',') return 0; j.p++; }
        char key[48]; if (!j_str(&j, key, sizeof key)) return 0;
        j_ws(&j); if (j.p >= j.end || *j.p != ':') return 0; j.p++;
        jmod m; j_mod(&j, &m);
        if (j.bad) return 0;
        if (!strcmp(key, "bodySliders") && m.has_on) c->body = m.on;
        else if (!strcmp(key, "ragdoll") && m.has_on) c->ragdoll = m.on;
        else if (!strcmp(key, "console")) { if (m.has_on) c->console = m.on; if (m.has_str && m.str[0]) snprintf(c->console_key, sizeof c->console_key, "%s", m.str); }
        else if (!strcmp(key, "menu")) { if (m.has_on) c->menu = m.on; if (m.has_str && key_vk(m.str)) snprintf(c->menu_key, sizeof c->menu_key, "%s", m.str); }
        else if (!strcmp(key, "fov")) { if (m.has_on) c->fov_on = m.on; if (m.has_num) c->fov = clampf((float)m.num, 60, 120); }
        else if (!strcmp(key, "fpsCap")) { if (m.has_on) c->fps_on = m.on; if (m.has_num) c->fps = (int)clampf((float)m.num, 0, 1000); }
        else if (!strcmp(key, "noPostEffects") && m.has_on) c->nofx = m.on;
        else if (!strcmp(key, "slowMotion")) { if (m.has_on) c->slomo_on = m.on; if (m.has_num) c->slomo = clampf((float)m.num, 0.1f, 1); }
        else if (!strcmp(key, "fly")) { if (m.has_on) c->fly_on = m.on; if (m.has_str) c->ghost = !_stricmp(m.str, "ghost"); }
        else if (!strcmp(key, "aimAssist")) { if (m.has_on) c->aim_on = m.on; if (m.has_num) c->aim_strength = (int)clampf((float)m.num, 1, 10); }
        else if (!strcmp(key, "aimCone") && m.has_num) c->aim_cone = (int)clampf((float)m.num, 5, 180);
        else if (!strcmp(key, "god") && m.has_on) c->god = m.on;
        else if (!strcmp(key, "cheatKeys") && m.has_on) c->keys = m.on;
        else if (!strcmp(key, "spawnBots")) { if (m.has_num) c->spawn_n = (int)clampf((float)m.num, 1, 20); if (m.side[0]) c->spawn_team = !_stricmp(m.side, "team"); }
    }
}

/* ---- the file, read on the worker thread; the game thread takes a copy when g_cfg_gen moves */
static char g_cfg_path[MAX_PATH];
static mods_cfg g_cfg_file;                /* the last one read (under g_cfg_lock) */
static volatile LONG g_cfg_gen;
static CRITICAL_SECTION g_cfg_lock;
static FILETIME g_cfg_mtime;
static void cfg_log(const char *why, const mods_cfg *c)
{
    logf_("config (%s): bodySliders %d, ragdoll %d, console %d (key %s), menu %d (key %s), fov %d/%.0f, fpsCap %d/%d, noPostEffects %d, slowMotion %d/%.2f, fly %d/%s, "
          "aimAssist %d/%d, aimCone %d, god %d, cheatKeys %d, spawnBots %d %s",
          why, c->body, c->ragdoll, c->console, c->console_key, c->menu, c->menu_key, c->fov_on, c->fov, c->fps_on, c->fps, c->nofx, c->slomo_on, c->slomo,
          c->fly_on, c->ghost ? "ghost" : "fly", c->aim_on, c->aim_strength, c->aim_cone, c->god, c->keys, c->spawn_n, c->spawn_team ? "team" : "enemy");
}
/* worker thread: read mods.json when it is new or changed; 1 = a new copy is there */
static int cfg_poll(void)
{
    if (!g_cfg_path[0]) return 0;
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(g_cfg_path, GetFileExInfoStandard, &a)) return 0;
    if (!CompareFileTime(&a.ftLastWriteTime, &g_cfg_mtime)) return 0;
    HANDLE h = CreateFileA(g_cfg_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    char buf[8192]; DWORD got = 0;
    BOOL ok = ReadFile(h, buf, sizeof buf - 1, &got, NULL);
    CloseHandle(h);
    if (!ok) return 0;
    g_cfg_mtime = a.ftLastWriteTime;
    mods_cfg c; cfg_defaults(&c);
    if (!cfg_parse(buf, got, &c)) { logf_("config: %s is not valid JSON (%lu bytes): defaults kept", g_cfg_path, got); return 0; }
    EnterCriticalSection(&g_cfg_lock); g_cfg_file = c; LeaveCriticalSection(&g_cfg_lock);
    InterlockedIncrement(&g_cfg_gen);
    cfg_log("mods.json", &c);
    return 1;
}
