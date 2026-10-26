/*
 * fnsettings.c - the game settings of fnmods.dll (#included by fnmods.c): field of view, frame rate cap, post effects,
 * slow motion and fly, from mods.json and the F8 menu, applied with the engine's own commands.
 *
 * Who owns what decides how each is applied: the client renders (its camera, frame rate and post-processing are its
 * own), the match server simulates (time and movement are the server's, and it corrects the client).
 *   - Field of view: the exec command "fov <deg>" (APlayerController::FOV -> PlayerCameraManager->SetFOV, a locked
 *     FOV; "fov 0" unlocks it). Read back with APlayerCameraManager::GetFOVAngle and logged.
 *   - Frame rate cap: the console variable t.MaxFPS (0 = none). The value it had is read first
 *     (UKismetSystemLibrary::GetConsoleVariableFloatValue) and put back when the cap is turned off.
 *   - Post effects off: the UE 4.26 console variables r.MotionBlurQuality, r.DepthOfFieldQuality, r.BloomQuality,
 *     r.LensFlareQuality, r.SceneColorFringeQuality (chromatic aberration) and r.Tonemapper.GrainQuantization set to 0
 *     (each read first and put back the same way). Set from the console they win over the game's scalability settings.
 *   - Slow motion: in the lobby (a standalone world) UGameplayStatics::SetGlobalTimeDilation on the client; in a match
 *     the match server's world is the one that counts: "slomo <x>" through FortPlayerController::ServerCheat, the RPC
 *     Project Reboot's server takes its cheat commands from (gameserver-reboot commands.cpp; the local player at
 *     127.0.0.1 is an operator there).
 *   - Fly: match only, "fly" / "ghost" (through walls) / "walk" through ServerCheat; the server puts the pawn in flying
 *     movement (UCheatManager::Fly's way: ACharacter::ClientCheatFly on the owning client). Fortnite's movement input is
 *     horizontal, so while flying Space and Ctrl add an up / down input (APawn::AddMovementInput, the client's own
 *     prediction sends it to the server like any input).
 * Every value is set again after a travel (a new player controller) and when the config changes.
 */

#define NFX 6
static const wchar_t *FX_CVAR[NFX] = { L"r.MotionBlurQuality", L"r.DepthOfFieldQuality", L"r.BloomQuality", L"r.LensFlareQuality",
                                        L"r.SceneColorFringeQuality", L"r.Tonemapper.GrainQuantization" };
static struct {
    int state;
    void *fpc;
    int o_pcm, o_pawn, o_cmc, o_mode;
    ufn getfov, cheat, standalone, timedil, addinput;
    int c_msg, t_ctx, t_val, s_ctx, s_ret, a_dir, a_scale, a_force, g_ret;
} SN;
static struct {
    void *pc; int32_t pc_serial; int standalone;
    float fov;                  /* applied: 0 = the game's */
    int fps; float fps_orig;    /* applied cap, -1 = the game's */
    int nofx; float fx_orig[NFX];
    float slomo;                /* applied, 1 = normal */
    void *fly_pawn; int fly;    /* applied: 0 walk, 1 fly, 2 ghost */
    DWORD fov_check;
} SA = { 0, 0, 0, 0, -1, NAN, 0, { 0 }, 1.0f, 0, 0, 0 };
static volatile int g_key_up, g_key_down;   /* Space / Ctrl held (the message hook sees them) */

static const need SETTINGS_NEEDS[] = {
    { 'f', "KismetSystemLibrary.ExecuteConsoleCommand" }, { 'f', "KismetSystemLibrary.GetConsoleVariableFloatValue", 1 },
    { 'f', "KismetSystemLibrary.IsStandalone" }, { 'f', "GameplayStatics.SetGlobalTimeDilation" },
    { 'p', "PlayerController.PlayerCameraManager" }, { 'f', "PlayerCameraManager.GetFOVAngle", 1 }, { 'f', "PlayerController.FOV" },
    { 'f', "FortPlayerController.ServerCheat" }, { 'p', "Controller.Pawn" }, { 'f', "Pawn.AddMovementInput" },
    { 'p', "Character.CharacterMovement", 1 }, { 'p', "CharacterMovementComponent.MovementMode", 1 },
};

static int settings_resolve(void)
{
    void *pcc = find_class("/Script/Engine", "PlayerController"), *ctl = find_class("/Script/Engine", "Controller");
    void *pcm = find_class("/Script/Engine", "PlayerCameraManager"), *fpc = find_class("/Script/FortniteGame", "FortPlayerController");
    void *pawn = find_class("/Script/Engine", "Pawn"), *chr = find_class("/Script/Engine", "Character");
    void *cmc = find_class("/Script/Engine", "CharacterMovementComponent");
    SN.fpc = fpc;
    SN.o_pcm = prop_off(pcc, "PlayerCameraManager"); SN.o_pawn = prop_off(ctl, "Pawn");
    SN.o_cmc = chr ? prop_off(chr, "CharacterMovement") : -1; SN.o_mode = cmc ? prop_off(cmc, "MovementMode") : -1;
    SN.getfov = fn_of(pcm, "GetFOVAngle"); SN.g_ret = parm(SN.getfov, "ReturnValue");
    SN.cheat = fn_of(fpc, "ServerCheat"); SN.c_msg = parm(SN.cheat, "Msg");
    SN.standalone = fn_of(AT(ksl(), O_CLASS, void *), "IsStandalone"); SN.s_ctx = parm(SN.standalone, "WorldContextObject"); SN.s_ret = parm(SN.standalone, "ReturnValue");
    SN.timedil = fn_of(gps() ? AT(gps(), O_CLASS, void *) : NULL, "SetGlobalTimeDilation"); SN.t_ctx = parm(SN.timedil, "WorldContextObject"); SN.t_val = parm(SN.timedil, "TimeDilation");
    SN.addinput = fn_of(pawn, "AddMovementInput"); SN.a_dir = parm(SN.addinput, "WorldDirection"); SN.a_scale = parm(SN.addinput, "ScaleValue"); SN.a_force = parm(SN.addinput, "bForce");
    logf_("settings: PlayerCameraManager %d, Pawn %d, CharacterMovement %d, MovementMode %d; GetFOVAngle %p, ServerCheat %p (Msg %d), IsStandalone %p, SetGlobalTimeDilation %p (%d %d), AddMovementInput %p (%d %d %d)",
          SN.o_pcm, SN.o_pawn, SN.o_cmc, SN.o_mode, SN.getfov.fn, SN.cheat.fn, SN.c_msg, SN.standalone.fn, SN.timedil.fn, SN.t_ctx, SN.t_val,
          SN.addinput.fn, SN.a_dir, SN.a_scale, SN.a_force);
    return SN.o_pcm >= 0 && SN.o_pawn >= 0 && SN.standalone.fn && SN.s_ret >= 0 ? 1 : -1;
}

static int is_standalone(void *pc)
{
    u8 p[512]; memset(p, 0, sizeof p);
    if (SN.s_ctx >= 0) AT(p, SN.s_ctx, void *) = pc;
    call(ksl(), SN.standalone, p);
    return AT(p, SN.s_ret, u8) != 0;
}
/* a cheat command for the match server (Project Reboot's ServerCheat) */
static int server_cheat(void *pc, const wchar_t *cmd)
{
    if (!SN.cheat.fn || SN.c_msg < 0) { logf_("settings: no ServerCheat: \"%ls\" not sent", cmd); return 0; }
    u8 p[512]; memset(p, 0, sizeof p);
    put_fstring(p, SN.c_msg, cmd);
    call(pc, SN.cheat, p);
    logf_("settings: server cheat \"%ls\" sent", cmd);
    return 1;
}
static float camera_fov(void *pc)
{
    void *cm = SN.o_pcm >= 0 ? AT(pc, SN.o_pcm, void *) : NULL;
    if (!cm || !SN.getfov.fn || SN.g_ret < 0) return NAN;
    u8 p[512]; memset(p, 0, sizeof p);
    call(cm, SN.getfov, p);
    return AT(p, SN.g_ret, float);
}
static int movement_mode(void *pawn)
{
    void *cmc = SN.o_cmc >= 0 ? AT(pawn, SN.o_cmc, void *) : NULL;
    return cmc && SN.o_mode >= 0 ? AT(cmc, SN.o_mode, u8) : -1;
}

static void settings_tick(void)
{
    static DWORD next;
    DWORD now = GetTickCount();
    void *pc = local_pc();
    if (!pc) return;
    if (SN.state == 0) { SN.state = settings_resolve(); if (SN.state < 0) logf_("settings: FAIL: engine types not found: settings off"); }
    if (SN.state < 0) return;
    /* only a Fortnite player controller in a world that has started: while a map loads the client has a plain
       PlayerController for a moment, and an exec command then crashed the game (in its own code, 14.30) */
    if (SN.fpc && !is_a(pc, SN.fpc)) return;
    if (pc != SA.pc || !alive(SA.pc, SA.pc_serial)) {
        /* a new world: the camera, the world's time and the pawn are new; the console variables stay */
        SA.pc = pc; SA.pc_serial = serial_of(pc); SA.standalone = is_standalone(pc);
        SA.fov = 0; SA.slomo = 1.0f; SA.fly_pawn = NULL; SA.fly = 0;
        char a[96]; logf_("settings: player controller %p (%s), %s", pc, obj_name(AT(pc, O_CLASS, void *), a, sizeof a), SA.standalone ? "standalone (lobby)" : "connected to a match server");
        next = 0;
    }
    /* fly: Space / Ctrl move up / down (every tick: the movement component consumes the input each frame) */
    if (SA.fly && SA.fly_pawn && SN.addinput.fn && SN.a_dir >= 0 && (g_key_up || g_key_down)) {
        u8 p[512]; memset(p, 0, sizeof p);
        float *d = (float *)(p + SN.a_dir); d[2] = g_key_up ? 1.0f : -1.0f;
        if (SN.a_scale >= 0) AT(p, SN.a_scale, float) = 1.0f;
        call(SA.fly_pawn, SN.addinput, p);
    }
    if (now < next) return;
    next = now + 250;
    /* in a match, nothing until the player has a pawn (the world is up and the camera is the match's) */
    if (!SA.standalone && !AT(pc, SN.o_pawn, void *)) return;
    wchar_t cmd[96];

    /* field of view */
    float fov = g_cfg.fov_on ? g_cfg.fov : 0.0f;
    if (fov != SA.fov) {
        swprintf(cmd, 96, L"fov %d", (int)fov); console(pc, cmd);
        SA.fov = fov; SA.fov_check = now + 1000;
    }
    if (SA.fov_check && now >= SA.fov_check) { SA.fov_check = 0; logf_("settings: fov asked %.0f, the camera reports %.1f", SA.fov, camera_fov(pc)); }

    /* frame rate cap */
    int fps = g_cfg.fps_on ? g_cfg.fps : -1;
    if (fps != SA.fps) {
        if (SA.fps < 0) SA.fps_orig = cvar_get(L"t.MaxFPS");
        if (fps >= 0) swprintf(cmd, 96, L"t.MaxFPS %d", fps);
        else if (SA.fps_orig == SA.fps_orig) swprintf(cmd, 96, L"t.MaxFPS %g", SA.fps_orig);
        else cmd[0] = 0;
        if (cmd[0]) console(pc, cmd);
        logf_("settings: t.MaxFPS %s %d (it was %.0f), now %.0f", fps >= 0 ? "capped at" : "back from", fps >= 0 ? fps : SA.fps, SA.fps_orig, cvar_get(L"t.MaxFPS"));
        SA.fps = fps;
    }

    /* post effects */
    if (g_cfg.nofx != SA.nofx) {
        char b[400]; int n = 0;
        for (int i = 0; i < NFX; i++) {
            if (g_cfg.nofx) SA.fx_orig[i] = cvar_get(FX_CVAR[i]);
            if (g_cfg.nofx) swprintf(cmd, 96, L"%ls 0", FX_CVAR[i]);
            else if (SA.fx_orig[i] == SA.fx_orig[i]) swprintf(cmd, 96, L"%ls %g", FX_CVAR[i], SA.fx_orig[i]);
            else continue;
            console(pc, cmd);
            n += snprintf(b + n, sizeof b - n, " %ls %.0f->%.0f", FX_CVAR[i], SA.fx_orig[i], cvar_get(FX_CVAR[i]));
        }
        logf_("settings: post effects %s:%s", g_cfg.nofx ? "off" : "back", b);
        SA.nofx = g_cfg.nofx;
    }

    /* slow motion */
    float slomo = g_cfg.slomo_on ? g_cfg.slomo : 1.0f;
    if (slomo != SA.slomo) {
        if (SA.standalone) {
            if (SN.timedil.fn && SN.t_ctx >= 0 && SN.t_val >= 0) {
                u8 p[512]; memset(p, 0, sizeof p);
                AT(p, SN.t_ctx, void *) = pc; AT(p, SN.t_val, float) = slomo;
                call(gps(), SN.timedil, p);
                logf_("settings: time dilation %.2f (lobby)", slomo);
            }
        } else {
            swprintf(cmd, 96, L"slomo %g", slomo); server_cheat(pc, cmd);
        }
        SA.slomo = slomo;
    }

    /* fly (match) */
    if (!SA.standalone) {
        void *pawn = AT(pc, SN.o_pawn, void *);
        int want = g_cfg.fly_on ? (g_cfg.ghost ? 2 : 1) : 0;
        if (pawn != SA.fly_pawn) { SA.fly_pawn = pawn; SA.fly = 0; }
        if (pawn && want != SA.fly) {
            server_cheat(pc, want == 2 ? L"ghost" : want == 1 ? L"fly" : L"walk");
            SA.fly = want;
            logf_("settings: pawn %p movement mode %d before", pawn, movement_mode(pawn));
        }
        static int mode_last = -2;   /* the movement mode while fly is on, logged when it changes */
        int mode = pawn && SA.fly ? movement_mode(pawn) : -2;
        if (mode != mode_last && mode != -2) logf_("settings: pawn %p movement mode %d (5 = flying)", pawn, mode);
        mode_last = mode;
    }
}
