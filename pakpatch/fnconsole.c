/*
 * fnconsole.c - the engine console in Fortnite 14.30: the "console" module of fnmods.dll (#included by fnmods.c).
 *
 * A Shipping build never creates the console (UE 4.26 UGameEngine::Init makes UGameViewportClient::ViewportConsole
 * only when !UE_BUILD_SHIPPING || ALLOW_CONSOLE_IN_SHIPPING), but the code that feeds it keys and draws it is there:
 * UGameViewportClient::InputKey hands every key to ViewportConsole first, and UGameViewportClient::Draw calls its
 * PostRender_Console. So the console comes back by creating one, as the console unlockers do:
 *   - UE4SS ConsoleEnablerMod: StaticConstructObject(Engine.ConsoleClass, GameViewport) into GameViewport.ViewportConsole,
 *     then ConsoleKeys[1].KeyName = "F10" on /Script/Engine.Default__InputSettings;
 *   - FortConsole (Fortnite): StaticConstructObject_Internal found by a code pattern, the same assignment;
 *   - OpenStorm: UGameplayStatics::SpawnObject(UConsole::StaticClass(), GameViewport).
 * Here it is OpenStorm's way, which needs no code pattern: UGameplayStatics::SpawnObject (a UFunction) called through
 * ProcessEvent on the game thread, with the engine's ConsoleClass (or /Script/Engine.Console) and the game viewport as
 * its outer, written into ViewportConsole (a UPROPERTY: the object stays referenced, the GC keeps it). Checked again
 * every half second, as UE4SS does on every new player controller. The console key: the first entry of
 * Default__InputSettings ConsoleKeys gets the configured key's name (mods.json "console" value, F10 by default:
 * the same on every keyboard layout); the array itself is never resized (its memory is the engine's).
 * Turned off (mods.json), the console object is taken out of ViewportConsole and the key is put back.
 */

static struct {
    int state;                  /* 0 not resolved, 1 resolved, -1 failed */
    void *engine_cls, *console_cls, *input_cdo, *key_struct;
    int o_gv, o_cc, o_vc, o_keys, key_size, key_name;
    ufn spawn; int sp_cls, sp_outer, sp_ret;
    void *engine; int32_t engine_serial;
    void *made; void *made_in;  /* the console we created and the viewport it is in */
    uint32_t key_orig[2]; int key_set; char key_now[24];
} CN;

static const need CONSOLE_NEEDS[] = {
    { 'p', "Engine.GameViewport" }, { 'p', "Engine.ConsoleClass", 1 }, { 'p', "GameViewportClient.ViewportConsole" },
    { 'c', "Console" }, { 'f', "GameplayStatics.SpawnObject" }, { 'p', "InputSettings.ConsoleKeys" }, { 'p', "Key.KeyName" },
};

static int console_resolve(void)
{
    CN.engine_cls = find_class("/Script/Engine", "Engine");
    void *gvc = find_class("/Script/Engine", "GameViewportClient");
    CN.console_cls = find_class("/Script/Engine", "Console");
    void *input = find_class("/Script/Engine", "InputSettings"), *statics = find_class("/Script/Engine", "GameplayStatics");
    CN.input_cdo = find_object_named("Default__InputSettings");
    CN.o_gv = prop_off(CN.engine_cls, "GameViewport");
    CN.o_cc = CN.engine_cls ? prop_off(CN.engine_cls, "ConsoleClass") : -1;
    CN.o_vc = gvc ? prop_off(gvc, "ViewportConsole") : -1;
    void *keys = input ? find_prop(input, "ConsoleKeys") : NULL;
    CN.o_keys = keys ? AT(keys, P_OFFSET, int32_t) : -1;
    CN.key_struct = keys ? array_struct_of(keys) : NULL;
    if (CN.key_struct) {
        int align = AT(CN.key_struct, S_MINALIGN, int32_t); if (align < 1) align = 1;
        CN.key_size = (AT(CN.key_struct, S_PROPSIZE, int32_t) + align - 1) / align * align;
        CN.key_name = prop_off(CN.key_struct, "KeyName");
    }
    CN.spawn = fn_of(statics, "SpawnObject");
    CN.sp_cls = parm(CN.spawn, "ObjectClass"); CN.sp_outer = parm(CN.spawn, "Outer"); CN.sp_ret = parm(CN.spawn, "ReturnValue");
    logf_("console: Engine %p (GameViewport %d, ConsoleClass %d), ViewportConsole %d, Console %p, SpawnObject %p (%d %d %d), Default__InputSettings %p (ConsoleKeys %d, FKey %d bytes, KeyName %d)",
          CN.engine_cls, CN.o_gv, CN.o_cc, CN.o_vc, CN.console_cls, CN.spawn.fn, CN.sp_cls, CN.sp_outer, CN.sp_ret, CN.input_cdo, CN.o_keys, CN.key_size, CN.key_name);
    return CN.engine_cls && CN.o_gv >= 0 && CN.o_vc >= 0 && CN.console_cls && CN.spawn.fn && CN.sp_cls >= 0 && CN.sp_outer >= 0 && CN.sp_ret >= 0 ? 1 : -1;
}

typedef struct { void **data; int32_t num, max; } keyarray;
/* the console keys as the engine has them, into the log */
static void console_keys_log(const char *when)
{
    if (!CN.input_cdo || CN.o_keys < 0 || CN.key_size <= 0 || CN.key_name < 0) { logf_("console: console keys unknown (%s)", when); return; }
    keyarray *a = (keyarray *)((u8 *)CN.input_cdo + CN.o_keys);
    char b[200] = ""; int n = 0;
    for (int i = 0; i < a->num && i < 8; i++) { char k[32]; name_str(AT((u8 *)a->data + (size_t)i * CN.key_size, CN.key_name, uint32_t), k, sizeof k); n += snprintf(b + n, sizeof b - n, " %s", k); }
    logf_("console: console keys %s: %d%s", when, a->num, b);
}
/* the configured key as the first console key (or the original back when key is NULL) */
static void console_key(const char *key)
{
    if (!CN.input_cdo || CN.o_keys < 0 || CN.key_size <= 0 || CN.key_name < 0) return;
    keyarray *a = (keyarray *)((u8 *)CN.input_cdo + CN.o_keys);
    if (a->num < 1 || !a->data) { if (key) logf_("console: no console keys in this build: the console opens from the F8 menu only"); return; }
    u8 *e0 = (u8 *)a->data;
    if (!key) {
        if (CN.key_set) { AT(e0, CN.key_name, uint32_t) = CN.key_orig[0]; AT(e0, CN.key_name + 4, uint32_t) = CN.key_orig[1]; CN.key_set = 0; CN.key_now[0] = 0; console_keys_log("restored"); }
        return;
    }
    if (!strcmp(CN.key_now, key)) return;
    for (const char *c = key; *c; c++) if (!((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '_')) { logf_("console: \"%s\" is not an engine key name", key); return; }
    wchar_t w[24]; int i = 0; for (; key[i] && i < 23; i++) w[i] = (wchar_t)key[i]; w[i] = 0;
    uint32_t n = fname_of(w);
    if (!n) { logf_("console: no name for key %s", key); return; }
    if (!CN.key_set) { CN.key_orig[0] = AT(e0, CN.key_name, uint32_t); CN.key_orig[1] = AT(e0, CN.key_name + 4, uint32_t); CN.key_set = 1; }
    AT(e0, CN.key_name, uint32_t) = n; AT(e0, CN.key_name + 4, uint32_t) = 0;
    snprintf(CN.key_now, sizeof CN.key_now, "%s", key);
    console_keys_log("now");
}

/* the virtual key of an engine key name (F1-F12, Tilde, a letter or digit); 0 = none */
static UINT key_vk(const char *k)
{
    if ((k[0] == 'F' || k[0] == 'f') && k[1] >= '1' && k[1] <= '9') { int n = atoi(k + 1); if (n >= 1 && n <= 12) return VK_F1 + n - 1; }
    if (!_stricmp(k, "Tilde")) return VK_OEM_3;
    if (k[0] && !k[1] && ((k[0] >= 'A' && k[0] <= 'Z') || (k[0] >= '0' && k[0] <= '9'))) return (UINT)k[0];
    return 0;
}
static HWND g_game_hwnd;
static void post_key(UINT vk)
{
    LPARAM sc = (LPARAM)MapVirtualKeyA(vk, MAPVK_VK_TO_VSC) << 16;
    PostMessageA(g_game_hwnd, WM_KEYDOWN, vk, 1 | sc);
    PostMessageA(g_game_hwnd, WM_KEYUP, vk, 1 | sc | (1u << 30) | (1u << 31));
}
/* open the console as its key does (the F8 menu, the test hooks): 1 = the key was sent */
static int console_open(void)
{
    UINT vk = CN.key_now[0] ? key_vk(CN.key_now) : 0;
    if (!vk || !g_game_hwnd || !CN.made) { logf_("console: cannot open (key %s, console %p)", CN.key_now[0] ? CN.key_now : "none", CN.made); return 0; }
    post_key(vk);
    return 1;
}

static void console_tick(void)
{
    static DWORD next;
    DWORD now = GetTickCount();
    if (now < next) return;
    next = now + 500;
    if (CN.state == 0) {
        static int tries;
        if (!local_pc()) return;   /* the engine's objects are there once a player controller is */
        CN.state = console_resolve();
        if (CN.state < 0 && ++tries < 6) { CN.state = 0; next = now + 5000; return; }
        if (CN.state < 0) { logf_("console: FAIL: engine types not found: no console"); return; }
        console_keys_log("of the build");
    }
    if (CN.state < 0) return;
    if (!CN.engine || !alive(CN.engine, CN.engine_serial)) {
        CN.engine = find_instance(CN.engine_cls);
        if (!CN.engine) return;
        CN.engine_serial = serial_of(CN.engine);
        char a[96]; logf_("console: engine %p (%s)", CN.engine, obj_name(AT(CN.engine, O_CLASS, void *), a, sizeof a));
    }
    void *gv = AT(CN.engine, CN.o_gv, void *);
    if (!gv) return;
    void **slot = (void **)((u8 *)gv + CN.o_vc);
    if (!g_cfg.console) {
        if (CN.made && *slot == CN.made) { *slot = NULL; logf_("console: turned off: console taken out of the viewport"); }
        CN.made = NULL;
        console_key(NULL);
        return;
    }
    if (!*slot) {
        void *cls = CN.o_cc >= 0 ? AT(CN.engine, CN.o_cc, void *) : NULL;
        if (!cls) cls = CN.console_cls;
        u8 p[512]; memset(p, 0, sizeof p);
        AT(p, CN.sp_cls, void *) = cls; AT(p, CN.sp_outer, void *) = gv;
        g_pe(gps(), CN.spawn.fn, p);
        void *c = AT(p, CN.sp_ret, void *);
        char a[96];
        if (!c || !is_a(c, CN.console_cls)) { logf_("console: SpawnObject(%s) gave %p: no console", obj_name(cls, a, sizeof a), c); CN.state = -1; return; }
        *slot = c; CN.made = c; CN.made_in = gv;
        logf_("console: created %p of class %s in viewport %p", c, obj_name(cls, a, sizeof a), gv);
    } else if (*slot != CN.made) {
        static void *seen;
        if (seen != *slot) { seen = *slot; logf_("console: the viewport has a console already (%p): kept", *slot); }
        CN.made = *slot;
    }
    console_key(g_cfg.console_key);
}
