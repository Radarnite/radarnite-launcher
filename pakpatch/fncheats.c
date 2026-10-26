/*
 * fncheats.c - the match cheats of fnmods.dll (#included by fnmods.c): aim assist, teleports, bot spawns and god mode,
 * from mods.json and the F8 menu. For the owner's offline 14.30 matches against his own bots on his own match server.
 *
 * What runs where: the client owns its camera, so the aim assist and the crosshair trace run here; the match server owns
 * the pawns, so teleports, spawns and god mode are cheat commands it runs (FortPlayerController::ServerCheat, Project
 * Reboot's commands, gameserver-reboot commands.cpp: tpto, tpground, tppoi, spawnbots, god).
 *   - Aim assist: while the right mouse button is held (the game window's WM_RBUTTONDOWN / UP, seen in fnmods.c's message
 *     hook), the camera turns to the head of the enemy nearest to the crosshair: the players of the game state's
 *     PlayerArray whose TeamIndex differs from the player's, alive (FortPawn.GetHealth > 0, not bIsDying), at most
 *     aimCone degrees off the camera's direction, in sight (a visibility trace from the camera to the head meets nothing
 *     else first). The head is the mesh's "head" socket (SceneComponent.GetSocketLocation). A target is kept while it stays
 *     valid. The turn is AController::SetControlRotation, at most strength * 60 degrees a second toward the head (so
 *     it keeps up with a moving target without lagging behind it); strength 10 snaps at once.
 *   - Teleport to the crosshair: KismetSystemLibrary.LineTraceSingle on the visibility channel from the camera along its
 *     direction (the pawn ignored); the hit point and normal go to the server ("tpto"), which puts the pawn's feet there.
 *   - Teleport to the map pin: the local AthenaMarkerComponent gets its own copy of its virtual table (fnmenu.c's way)
 *     whose ProcessEvent records the WorldPosition of every ServerAddMapMarker the client sends; "tpground X Y Z" then
 *     makes the server find the ground at that X Y (a trace down on the server's world, which has all of it loaded).
 *   - Random POI: "tppoi" (the server picks one).
 *   - Spawn bots: "spawnbots <n> enemy|team" (the server spawns them around the player with the match's bot code).
 *   - God mode: "god on" / "god off", sent again for every new pawn.
 * Keys (cheatKeys on, in a match): F6 teleport to the crosshair, F7 to the map pin, F9 spawn bots; taken from the game.
 */

static struct {
    int state;
    int o_hit_actor, o_owner;
    int o_crot, o_parray, o_pspawn, o_team, o_mesh, o_capsule, o_marker, o_req, o_wpos, o_mtype, o_hit_imp, o_hit_nrm;
    boolp dying, can_damage;
    ufn setrot, camloc, camrot, gamestate, socket, health, actorloc, trace, halfh, addmarker;
    int sr_rot, cl_ret, cr_ret, gs_ctx, gs_ret, so_name, so_ret, h_ret, al_ret;
    int t_ctx, t_start, t_end, t_chan, t_cplx, t_hit, t_self, t_ret, hh_ret, am_req;
    uint32_t head;
    void *marker; int32_t marker_serial;
} CH;
static volatile int g_rmb;
static int g_aim_trace;                     /* the cheats test: every aim step logged (first 25 of a press) */                  /* right mouse button held (message hook) */
static volatile int g_ch_action;            /* a key's action for the next tick: 1 crosshair, 2 map pin, 3 spawn, 4 POI */
static struct {
    void *pawn; int god;                    /* god mode sent for this pawn */
    void *target; int32_t target_serial;    /* the aim's target */
    DWORD aim_from, aim_last; int aim_logged; float aim_err0;
    int has_pin; float pin[3]; DWORD pin_at;
    int has_tp; float tp[3];                /* the last teleport's target (feet), for the test's measure */
} CA;

static const need CHEATS_NEEDS[] = {
    { 'p', "Controller.ControlRotation" }, { 'f', "Controller.SetControlRotation" },
    { 'f', "PlayerCameraManager.GetCameraLocation" }, { 'f', "PlayerCameraManager.GetCameraRotation" }, { 'f', "GameplayStatics.GetGameState" },
    { 'p', "GameStateBase.PlayerArray" }, { 'p', "PlayerState.PawnPrivate" }, { 'p', "FortPlayerStateAthena.TeamIndex" },
    { 'f', "SceneComponent.GetSocketLocation" }, { 'f', "FortPawn.GetHealth" }, { 'p', "FortPawn.bIsDying", 1 },
    { 'f', "KismetSystemLibrary.LineTraceSingle" }, { 'c', "HitResult" }, { 'p', "FortPlayerControllerAthena.MarkerComponent", 1 },
    { 'f', "AthenaMarkerComponent.ServerAddMapMarker", 1 }, { 'p', "FortClientMarkerRequest.WorldPosition", 1 },
    { 'f', "CapsuleComponent.GetScaledCapsuleHalfHeight", 1 }, { 'p', "Actor.bCanBeDamaged", 1 },
};

/* ---- small vector helpers (UE: FVector / FRotator of floats, degrees) */
typedef struct { float x, y, z; } v3;
#define RAD 0.017453292519943295f
static v3 rot_dir(const float *r) { float p = r[0] * RAD, y = r[1] * RAD; v3 d = { cosf(p) * cosf(y), cosf(p) * sinf(y), sinf(p) }; return d; }
static float norm180(float a) { a = fmodf(a, 360.0f); if (a > 180) a -= 360; if (a <= -180) a += 360; return a; }
static float v_len(v3 a) { return sqrtf(a.x * a.x + a.y * a.y + a.z * a.z); }
static float angle_between(v3 a, v3 b)
{
    float la = v_len(a), lb = v_len(b);
    if (la <= 0 || lb <= 0) return 180;
    float c = (a.x * b.x + a.y * b.y + a.z * b.z) / (la * lb);
    return acosf(c > 1 ? 1 : c < -1 ? -1 : c) / RAD;
}

static int cheats_resolve(void)
{
    void *ctl = find_class("/Script/Engine", "Controller"), *pcm = find_class("/Script/Engine", "PlayerCameraManager");
    void *gsb = find_class("/Script/Engine", "GameStateBase"), *ps = find_class("/Script/Engine", "PlayerState");
    void *fpsa = find_class("/Script/FortniteGame", "FortPlayerStateAthena"), *chr = find_class("/Script/Engine", "Character");
    void *scn = find_class("/Script/Engine", "SceneComponent"), *fpawn = find_class("/Script/FortniteGame", "FortPawn");
    void *actor = find_class("/Script/Engine", "Actor"), *cap = find_class("/Script/Engine", "CapsuleComponent");
    void *fpca = find_class("/Script/FortniteGame", "FortPlayerControllerAthena"), *amc = find_class("/Script/FortniteGame", "AthenaMarkerComponent");
    CH.o_crot = ctl ? prop_off(ctl, "ControlRotation") : -1;
    CH.setrot = fn_of(ctl, "SetControlRotation"); CH.sr_rot = parm(CH.setrot, "NewRotation");
    CH.camloc = fn_of(pcm, "GetCameraLocation"); CH.cl_ret = parm(CH.camloc, "ReturnValue");
    CH.camrot = fn_of(pcm, "GetCameraRotation"); CH.cr_ret = parm(CH.camrot, "ReturnValue");
    CH.gamestate = fn_of(gps() ? AT(gps(), O_CLASS, void *) : NULL, "GetGameState"); CH.gs_ctx = parm(CH.gamestate, "WorldContextObject"); CH.gs_ret = parm(CH.gamestate, "ReturnValue");
    CH.o_parray = gsb ? prop_off(gsb, "PlayerArray") : -1; CH.o_pspawn = ps ? prop_off(ps, "PawnPrivate") : -1;
    CH.o_team = fpsa ? prop_off(fpsa, "TeamIndex") : -1;
    CH.o_mesh = chr ? prop_off(chr, "Mesh") : -1; CH.o_capsule = chr ? prop_off(chr, "CapsuleComponent") : -1;
    CH.socket = fn_of(scn, "GetSocketLocation"); CH.so_name = parm(CH.socket, "InSocketName"); CH.so_ret = parm(CH.socket, "ReturnValue");
    CH.health = fn_of(fpawn, "GetHealth"); CH.h_ret = parm(CH.health, "ReturnValue");
    CH.dying = bool_prop(fpawn, "bIsDying"); CH.can_damage = bool_prop(actor, "bCanBeDamaged");
    CH.actorloc = fn_of(actor, "K2_GetActorLocation"); CH.al_ret = parm(CH.actorloc, "ReturnValue");
    CH.trace = fn_of(AT(ksl(), O_CLASS, void *), "LineTraceSingle");
    CH.t_ctx = parm(CH.trace, "WorldContextObject"); CH.t_start = parm(CH.trace, "Start"); CH.t_end = parm(CH.trace, "End");
    CH.t_chan = parm(CH.trace, "TraceChannel"); CH.t_cplx = parm(CH.trace, "bTraceComplex"); CH.t_hit = parm(CH.trace, "OutHit");
    CH.t_self = parm(CH.trace, "bIgnoreSelf"); CH.t_ret = parm(CH.trace, "ReturnValue");
    void *hr = CH.trace.fn ? struct_of(find_prop(CH.trace.fn, "OutHit")) : NULL;
    CH.o_hit_imp = hr ? prop_off(hr, "ImpactPoint") : -1; CH.o_hit_nrm = hr ? prop_off(hr, "ImpactNormal") : -1;
    CH.o_hit_actor = hr ? prop_off(hr, "Actor") : -1; CH.o_owner = actor ? prop_off(actor, "Owner") : -1;
    CH.halfh = fn_of(cap, "GetScaledCapsuleHalfHeight"); CH.hh_ret = parm(CH.halfh, "ReturnValue");
    CH.o_marker = fpca ? prop_off(fpca, "MarkerComponent") : -1;
    CH.addmarker = fn_of(amc, "ServerAddMapMarker"); CH.am_req = parm(CH.addmarker, "MarkerRequest");
    void *req = CH.addmarker.fn ? struct_of(find_prop(CH.addmarker.fn, "MarkerRequest")) : NULL;
    static const char *const POS[] = { "WorldPosition", "BasePosition", "Position", "Location", "WorldLocation" };
    CH.o_wpos = -1;
    for (int k = 0; req && k < (int)(sizeof POS / sizeof POS[0]) && CH.o_wpos < 0; k++) CH.o_wpos = prop_off(req, POS[k]);
    CH.o_mtype = req ? prop_off(req, "MarkerType") : -1;
    if (req) {   /* the request's fields as 14.30 has them (the position's name differs between versions) */
        char b[900]; int n = 0;
        for (void *f = AT(req, S_CHILDPROPS, void *); f && n < 820; f = AT(f, FF_NEXT, void *)) {
            char nm[64]; if (!name_str(AT(f, FF_NAME, uint32_t), nm, sizeof nm)) snprintf(nm, sizeof nm, "?");
            n += snprintf(b + n, sizeof b - n, " %s@0x%x", nm, AT(f, P_OFFSET, int32_t));
        }
        logf_("cheats: FortClientMarkerRequest:%s", n ? b : " (no fields)");
    }
    CH.head = fname_of(L"head");
    logf_("cheats: ControlRotation %d, SetControlRotation %p (%d), camera %p/%p, GetGameState %p, PlayerArray %d, "
          "PawnPrivate %d, TeamIndex %d, Mesh %d, Capsule %d, GetSocketLocation %p (%d %d), GetHealth %p, bIsDying %d, LineTraceSingle %p (hit %d: impact %d normal %d actor %d), "
          "MarkerComponent %d, ServerAddMapMarker %p (request %d: WorldPosition %d, MarkerType %d), half height %p, bCanBeDamaged %d, head name %u",
          CH.o_crot, CH.setrot.fn, CH.sr_rot, CH.camloc.fn, CH.camrot.fn, CH.gamestate.fn, CH.o_parray,
          CH.o_pspawn, CH.o_team, CH.o_mesh, CH.o_capsule, CH.socket.fn, CH.so_name, CH.so_ret, CH.health.fn, CH.dying.off, CH.trace.fn, CH.t_hit,
          CH.o_hit_imp, CH.o_hit_nrm, CH.o_hit_actor, CH.o_marker, CH.addmarker.fn, CH.am_req, CH.o_wpos, CH.o_mtype, CH.halfh.fn, CH.can_damage.off, CH.head);
    int ok = CH.o_crot >= 0 && CH.setrot.fn && CH.sr_rot >= 0 && CH.camloc.fn && CH.cl_ret >= 0 && CH.camrot.fn && CH.cr_ret >= 0 &&
             CH.gamestate.fn && CH.gs_ret >= 0 && CH.o_parray >= 0 && CH.o_pspawn >= 0;
    return ok ? 1 : -1;
}

/* ---- engine reads */
static int cam_view(void *pc, v3 *loc, float rot[3])
{
    void *cm = SN.o_pcm >= 0 ? AT(pc, SN.o_pcm, void *) : NULL;
    if (!cm) return 0;
    u8 p[512]; memset(p, 0, sizeof p); call(cm, CH.camloc, p); *loc = *(v3 *)(p + CH.cl_ret);
    memset(p, 0, sizeof p); call(cm, CH.camrot, p); memcpy(rot, p + CH.cr_ret, 12);
    return 1;
}
static v3 actor_loc(void *a) { v3 r = { 0, 0, 0 }; u8 p[256]; memset(p, 0, sizeof p); if (CH.al_ret >= 0) { call(a, CH.actorloc, p); r = *(v3 *)(p + CH.al_ret); } return r; }
static float pawn_half_height(void *pawn)
{
    void *cap = CH.o_capsule >= 0 ? AT(pawn, CH.o_capsule, void *) : NULL;
    if (!cap || !CH.halfh.fn || CH.hh_ret < 0) return NAN;
    u8 p[256]; memset(p, 0, sizeof p); call(cap, CH.halfh, p);
    return AT(p, CH.hh_ret, float);
}
static float pawn_health(void *pawn)
{
    if (!CH.health.fn || CH.h_ret < 0) return NAN;
    u8 p[256]; memset(p, 0, sizeof p); call(pawn, CH.health, p);
    return AT(p, CH.h_ret, float);
}
static int team_of(void *ps) { return CH.o_team >= 0 && ps ? AT(ps, CH.o_team, u8) : -1; }
static void *my_player_state(void *pawn)
{
    static int o_ps = -2;
    if (o_ps == -2) o_ps = prop_off(find_class("/Script/Engine", "Pawn"), "PlayerState");
    return o_ps >= 0 && pawn ? AT(pawn, o_ps, void *) : NULL;
}
/* the game state's players: count of entries, each a player state */
static int player_array(void *pc, void ***arr)
{
    u8 p[256]; memset(p, 0, sizeof p);
    if (CH.gs_ctx >= 0) AT(p, CH.gs_ctx, void *) = pc;
    call(gps(), CH.gamestate, p);
    void *gs = AT(p, CH.gs_ret, void *);
    if (!gs) return 0;
    *arr = AT(gs, CH.o_parray, void **);
    int n = AT(gs, CH.o_parray + 8, int32_t);
    return *arr && n > 0 && n < 1000 ? n : 0;
}
static int pawn_alive(void *pawn) { if (CH.dying.off >= 0 && bool_get(pawn, CH.dying)) return 0; float h = pawn_health(pawn); return !(h <= 0); }
static v3 head_of(void *pawn)
{
    void *mesh = CH.o_mesh >= 0 ? AT(pawn, CH.o_mesh, void *) : NULL;
    if (mesh && CH.socket.fn && CH.so_name >= 0 && CH.so_ret >= 0 && CH.head) {
        u8 p[256]; memset(p, 0, sizeof p); AT(p, CH.so_name, uint32_t) = CH.head; call(mesh, CH.socket, p);
        return *(v3 *)(p + CH.so_ret);
    }
    return actor_loc(pawn);
}
/* a visibility-channel trace (KismetSystemLibrary.LineTraceSingle) with the pawn `self` ignored: 1 = hit, its point,
   normal and actor's object index */
static int trace(void *self, v3 from, v3 to, v3 *pt, v3 *nrm, int32_t *actor)
{
    if (!CH.trace.fn || CH.t_hit < 0 || CH.t_ret < 0 || CH.o_hit_imp < 0) return 0;
    u8 p[512]; memset(p, 0, sizeof p);
    AT(p, CH.t_ctx, void *) = self;         /* bIgnoreSelf ignores the world context object: the player's pawn */
    *(v3 *)(p + CH.t_start) = from; *(v3 *)(p + CH.t_end) = to;
    if (CH.t_chan >= 0) AT(p, CH.t_chan, u8) = 0;   /* TraceTypeQuery1 = the visibility channel */
    if (CH.t_self >= 0) AT(p, CH.t_self, u8) = 1;
    call(ksl(), CH.trace, p);
    if (!AT(p, CH.t_ret, u8)) return 0;
    if (pt) *pt = *(v3 *)(p + CH.t_hit + CH.o_hit_imp);
    if (nrm) *nrm = CH.o_hit_nrm >= 0 ? *(v3 *)(p + CH.t_hit + CH.o_hit_nrm) : (v3){ 0, 0, 1 };
    if (actor) *actor = CH.o_hit_actor >= 0 ? AT(p, CH.t_hit + CH.o_hit_actor, int32_t) : -1;   /* TWeakObjectPtr: ObjectIndex first */
    return 1;
}
/* in sight: nothing between the camera and the head but the target itself, what it owns (its weapon, its back bling)
   or something within 30 cm of the head (a hat's own actor) */
static int head_visible(void *me, void *other, v3 cam, v3 head)
{
    int32_t a = -1; v3 hp;
    if (!trace(me, cam, head, &hp, NULL, &a)) return 1;
    if (a == AT(other, O_INDEX, int32_t)) return 1;
    v3 d = { hp.x - head.x, hp.y - head.y, hp.z - head.z };
    if (v_len(d) < 30) return 1;
    void *o = a >= 0 ? obj_at(a, NULL) : NULL;
    return o && CH.o_owner >= 0 && AT(o, CH.o_owner, void *) == other;
}
static int g_aim_in_cone;   /* the last pick: enemies within the cone (in sight or not) */
/* the enemy to aim at: alive, another team, within the cone, in sight; the one nearest to the crosshair */
static void *aim_pick(void *pc, void *me, v3 cam, v3 fwd, float cone, float *err)
{
    void **arr; int n = player_array(pc, &arr);
    int my_team = team_of(my_player_state(me));
    void *best = NULL; float best_a = cone; int in_cone = 0;
    for (int i = 0; i < n; i++) {
        void *ps = arr[i]; void *pawn = ps ? AT(ps, CH.o_pspawn, void *) : NULL;
        if (!pawn || pawn == me) continue;
        if (my_team >= 0 && team_of(ps) == my_team) continue;
        if (!pawn_alive(pawn)) continue;
        v3 h = head_of(pawn), d = { h.x - cam.x, h.y - cam.y, h.z - cam.z };
        float a = angle_between(fwd, d);
        if (a <= cone) in_cone++;
        if (a > best_a) continue;
        if (!head_visible(me, pawn, cam, h)) continue;
        best = pawn; best_a = a;
    }
    if (err) *err = best_a;
    g_aim_in_cone = in_cone;
    return best;
}

/* ---- the aim assist (every tick while the right button is held) */
static void aim_tick(void *pc, void *me)
{
    int rmb = g_rmb || (GetForegroundWindow() == g_game_hwnd && (GetAsyncKeyState(VK_RBUTTON) & 0x8000));
    DWORD now = GetTickCount();
    if (!rmb || !g_cfg.aim_on) {
        if (CA.aim_from && CA.aim_logged == 0) logf_("aim: released after %lu ms without reaching a target (start error %.1f deg)", now - CA.aim_from, CA.aim_err0);
        CA.aim_from = 0; CA.target = NULL;
        return;
    }
    v3 cam; float rot[3];
    if (!cam_view(pc, &cam, rot)) return;
    v3 fwd = rot_dir(rot);
    void *t = CA.target && alive(CA.target, CA.target_serial) && pawn_alive(CA.target) ? CA.target : NULL;
    if (t) { v3 h = head_of(t), d = { h.x - cam.x, h.y - cam.y, h.z - cam.z }; if (angle_between(fwd, d) > g_cfg.aim_cone || !head_visible(me, t, cam, h)) t = NULL; }
    if (!t) { float e; t = aim_pick(pc, me, cam, fwd, (float)g_cfg.aim_cone, &e); if (t) { CA.target = t; CA.target_serial = serial_of(t); } }
    if (!CA.aim_from) { CA.aim_from = now; CA.aim_last = now; CA.aim_logged = 0; CA.aim_err0 = NAN; }
    if (!t) {
        if (CA.aim_logged == 0 && now - CA.aim_from > 300) { CA.aim_logged = -1; logf_("aim: no target 300 ms after the button: %d enemies in the cone, none in sight", g_aim_in_cone); }
        CA.aim_last = now;
        return;
    }
    if (CA.aim_logged < 0) CA.aim_logged = 0;
    v3 h = head_of(t), d = { h.x - cam.x, h.y - cam.y, h.z - cam.z };
    float dist = v_len(d), err = angle_between(fwd, d);
    if (CA.aim_err0 != CA.aim_err0) {
        CA.aim_err0 = err;
        char b[96]; v3 at = actor_loc(t);
        logf_("aim: target %p (%s) %.1f m away, %.1f deg off the crosshair, head %.0f cm above its actor location", t,
              obj_name(AT(t, O_CLASS, void *), b, sizeof b), dist / 100, err, h.z - at.z);
    }
    /* on target: the crosshair's ray passes within 5 cm of the head socket */
    float miss = dist * sinf(err * RAD);
    if (!CA.aim_logged && miss < 5.0f) { CA.aim_logged = 1; logf_("aim: on the head after %lu ms (miss %.1f cm, %.2f deg, strength %d)", now - CA.aim_from, miss, err, g_cfg.aim_strength); }
    float want_p = atan2f(d.z, sqrtf(d.x * d.x + d.y * d.y)) / RAD, want_y = atan2f(d.y, d.x) / RAD;
    /* step from what the camera shows (ControlRotation reads degrees off it, its roll too, while the client's input runs),
       written in ControlRotation's own winding so a 357 vs -3 never spins the camera */
    float *cr = (float *)((u8 *)pc + CH.o_crot);
    float bp = cr[0] + norm180(rot[0] - cr[0]), by = cr[1] + norm180(rot[1] - cr[1]);
    float dp = norm180(want_p - rot[0]), dy = norm180(want_y - rot[1]);
    float dt = (float)(now - CA.aim_last) / 1000.0f; if (dt > 0.1f) dt = 0.1f;
    CA.aim_last = now;
    float left = sqrtf(dp * dp + dy * dy), step = g_cfg.aim_strength * 60.0f * dt;
    float f = g_cfg.aim_strength >= 10 || left <= step ? 1.0f : step / left;
    u8 p[256]; memset(p, 0, sizeof p);
    float *nr = (float *)(p + CH.sr_rot);
    nr[0] = bp + dp * f; nr[1] = by + dy * f; nr[2] = cr[2] + norm180(rot[2] - cr[2]);   /* the roll the camera shows too */
    static int traced; static DWORD traced_from;
    if (g_aim_trace && traced_from != CA.aim_from) { traced_from = CA.aim_from; traced = 0; }
    if (g_aim_trace && traced++ < 25)
        logf_("aim trace: camera %.1f %.1f %.1f, control %.1f %.1f %.1f, want %.1f %.1f, error %.2f deg, dt %.3f, f %.2f -> %.1f %.1f %.1f",
              rot[0], rot[1], rot[2], cr[0], cr[1], cr[2], want_p, want_y, err, dt, f, nr[0], nr[1], nr[2]);
    call(pc, CH.setrot, p);
}

/* ---- teleports, spawns, god (server cheats) */
static int crosshair_hit(void *pc, void *me, v3 *pt, v3 *nrm)
{
    v3 cam; float rot[3];
    if (!cam_view(pc, &cam, rot)) return 0;
    v3 fwd = rot_dir(rot);
    const float far_ = 1048576.0f;          /* HALF_WORLD_MAX (UE 4.26 EngineDefines.h): the whole world */
    v3 end = { cam.x + fwd.x * far_, cam.y + fwd.y * far_, cam.z + fwd.z * far_ };
    return trace(me, cam, end, pt, nrm, NULL);
}
static void tp_crosshair(void *pc, void *me)
{
    v3 h, n;
    if (!crosshair_hit(pc, me, &h, &n)) { logf_("teleport: the crosshair hits nothing: not sent"); return; }
    wchar_t cmd[160]; swprintf(cmd, 160, L"tpto %.1f %.1f %.1f %.3f %.3f %.3f", h.x, h.y, h.z, n.x, n.y, n.z);
    CA.has_tp = 1; CA.tp[0] = h.x; CA.tp[1] = h.y; CA.tp[2] = h.z;
    v3 at = actor_loc(me);
    logf_("teleport: crosshair at %.0f %.0f %.0f (normal %.2f %.2f %.2f), %.1f m from the pawn", h.x, h.y, h.z, n.x, n.y, n.z,
          sqrtf((h.x - at.x) * (h.x - at.x) + (h.y - at.y) * (h.y - at.y) + (h.z - at.z) * (h.z - at.z)) / 100);
    server_cheat(pc, cmd);
}
static void tp_pin(void *pc)
{
    if (!CA.has_pin) { logf_("teleport: no map pin placed in this match"); return; }
    wchar_t cmd[128]; swprintf(cmd, 128, L"tpground %.1f %.1f %.1f", CA.pin[0], CA.pin[1], CA.pin[2]);
    CA.has_tp = 2; memcpy(CA.tp, CA.pin, sizeof CA.tp);
    server_cheat(pc, cmd);
}
static void spawn_bots(void *pc)
{
    wchar_t cmd[64]; swprintf(cmd, 64, L"spawnbots %d %ls", g_cfg.spawn_n, g_cfg.spawn_team ? L"team" : L"enemy");
    server_cheat(pc, cmd);
}

/* ---- the map pin: the marker component's ProcessEvent sees the client's ServerAddMapMarker */
typedef struct { void **orig, **clone; process_event_fn pe; } cvt;
static cvt g_cvt[2]; static int g_ncvt;
static void marker_pe_hook(void *obj, void *fn, void *parms)
{
    void **vt = *(void ***)obj;
    process_event_fn pe = g_pe;
    for (int k = 0; k < g_ncvt; k++) if (g_cvt[k].clone == vt) { pe = g_cvt[k].pe; break; }
    if (fn == CH.addmarker.fn && parms && CH.am_req >= 0 && CH.o_wpos >= 0) {
        float *w = (float *)((u8 *)parms + CH.am_req + CH.o_wpos);
        CA.has_pin = 1; CA.pin[0] = w[0]; CA.pin[1] = w[1]; CA.pin[2] = w[2]; CA.pin_at = GetTickCount();
        logf_("map pin: marker at %.0f %.0f %.0f (type %d)", w[0], w[1], w[2], CH.o_mtype >= 0 ? AT(parms, CH.am_req + CH.o_mtype, u8) : -1);
    }
    pe(obj, fn, parms);
}
static int marker_swap(void *o)
{
    void **vt = AT(o, 0, void **);
    for (int k = 0; k < g_ncvt; k++) if (g_cvt[k].clone == vt) return 1;
    cvt *v = NULL;
    for (int k = 0; k < g_ncvt; k++) if (g_cvt[k].orig == vt) v = &g_cvt[k];
    if (!v) {
        if (g_ncvt >= (int)(sizeof g_cvt / sizeof g_cvt[0])) return 0;
        void *e = vt[PE_INDEX];
        if (e != (void *)g_pe) { logf_("map pin: vtable %p: entry 0x%x %p is not ProcessEvent, left alone", vt, PE_INDEX, e); return 0; }
        void **mem = VirtualAlloc(NULL, (VT_SLOTS + 1) * sizeof(void *), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!mem) return 0;
        int n = 0;
        while (n < VT_SLOTS + 1 && rd(vt - 1 + n, &mem[n], sizeof(void *))) n++;
        if (n < PE_INDEX + 2) { VirtualFree(mem, 0, MEM_RELEASE); return 0; }
        mem[1 + PE_INDEX] = (void *)marker_pe_hook;
        v = &g_cvt[g_ncvt++]; v->orig = vt; v->clone = mem + 1; v->pe = (process_event_fn)e;
        logf_("map pin: marker component vtable %p copied (%d entries), ProcessEvent watched for ServerAddMapMarker", vt, n - 1);
    }
    AT(o, 0, void **) = v->clone;
    return 1;
}

/* ---- keys (message hook, game thread): 1 = the game does not get it */
static int cheats_key(UINT msg, WPARAM vk)
{
    if (msg == WM_RBUTTONDOWN) { g_rmb = 1; return 0; }
    if (msg == WM_RBUTTONUP) { g_rmb = 0; return 0; }
    if (!g_cfg.keys || CH.state <= 0 || SA.standalone || !SA.pc) return 0;
    int a = vk == VK_F6 ? 1 : vk == VK_F7 ? 2 : vk == VK_F9 ? 3 : 0;
    if (!a) return 0;
    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) g_ch_action = a;
    return 1;
}

static void cheats_tick(void)
{
    void *pc = SA.pc;
    if (!pc || !alive(pc, SA.pc_serial) || SA.standalone) { CA.pawn = NULL; CA.has_pin = 0; CA.target = NULL; return; }
    if (CH.state == 0) { CH.state = cheats_resolve(); if (CH.state < 0) logf_("cheats: FAIL: engine types not found: cheats off"); }
    if (CH.state < 0) return;
    void *me = AT(pc, SN.o_pawn, void *);
    if (!me) return;
    /* the marker component of this controller, watched once */
    void *mc = CH.o_marker >= 0 ? AT(pc, CH.o_marker, void *) : NULL;
    if (mc && CH.addmarker.fn && (mc != CH.marker || !alive(CH.marker, CH.marker_serial))) {
        CH.marker = mc; CH.marker_serial = serial_of(mc);
        marker_swap(mc);
    }
    /* god mode for this pawn */
    if (me != CA.pawn) { CA.pawn = me; CA.god = 0; }
    if (g_cfg.god != CA.god) { server_cheat(pc, g_cfg.god ? L"god on" : L"god off"); CA.god = g_cfg.god; }
    int a = g_ch_action; g_ch_action = 0;
    if (a == 1) tp_crosshair(pc, me);
    else if (a == 2) tp_pin(pc);
    else if (a == 3) spawn_bots(pc);
    else if (a == 4) server_cheat(pc, L"tppoi");
    aim_tick(pc, me);
}
