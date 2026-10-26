/*
 * fnragdoll.c - the ragdoll deaths module of fnmods.dll (#included by fnmods.c, which owns the engine access: object
 * array, names, reflection, ProcessEvent, the game-thread tick and the fault guard).
 *
 * The game plays a "digitize" effect when a player dies: the pawn's blueprint (PlayerPawn_Athena_C.OnDeathPlayEffects)
 * swaps the character's materials for dissolving ones and sends a beam to the sky, and the body vanishes. Undoing that
 * afterwards does not work (measured: the beam and the glow still play), so the event is not let through: every other
 * player's pawn gets its own copy of its class's virtual table whose ProcessEvent entry drops OnDeathPlayEffects (an
 * object's vtable pointer is data; no game code is patched). This module watches every FortPawn of the client; the
 * frame a pawn's replicated bIsDying turns on it:
 *   - undoes what is left of the effect (CleanupDissolveFX, RestoreCharacterMats) and keeps the pawn visible (the
 *     server's bIsHiddenForDeath hides it);
 *   - turns the character's base mesh into a ragdoll: capsule without collision, movement off, the mesh on the
 *     "Ragdoll" collision profile with all its bodies simulated (its physics asset; the part meshes follow it through
 *     the master pose, imported outfits included), plus a small push from the hit direction;
 *   - after RAGDOLL_HOLD_MS lets it sink through the floor (collision off, gravity on) and hides it.
 * The pawn actor itself is the server's: it removes it about 3 s after the death unless the match server sets a longer
 * life span (gameserver-reboot fnoffline.h, FNOFFLINE_CORPSE_SECONDS). The player's own pawn keeps the game's effect.
 * Everything is logged to fnmods.log with a "ragdoll:" prefix, including the events the game runs on a dying pawn.
 *
 * mods.json "ragdoll" or FNRAGDOLL=0 turns it off (the game's own effect), FNRAGDOLL=observe logs what happens at each death without touching
 * it, FNRAGDOLL=alternate does observe / ragdoll on every other death (test runs).
 */

#define RAGDOLL_HOLD_MS 6000
#define RAGDOLL_SINK_MS 1500
#define MAXP 128

enum { RD_OFF, RD_ON, RD_OBSERVE, RD_ALTERNATE };
static int g_rd_mode = RD_ON;
static void *g_skip_fn[4]; static int g_nskip;   /* OnDeathPlayEffects UFunctions (pe_hook drops them) */

static struct {
    int ok;
    void *fort_pawn, *character, *actor, *scene, *prim, *skc, *skinned, *skm, *physasset, *move;
    boolp is_dying, hidden_for_death, actor_hidden, comp_hidden, comp_visible, blend_physics;
    int o_mesh, o_capsule, o_movement, o_skelmesh, o_physasset, o_pa_override, o_bodies, o_children, o_velocity, o_controller;
    ufn set_hidden_actor, destroy, set_sim, set_all_sim, set_profile, set_coll, wake, impulse, set_gravity, disable_move,
        stop_move, set_vis_comp, set_hidden_comp, set_pawn_vis, str_to_name, set_physasset;
    void *fallback_pa; int fallback_tried;   /* the default body's physics asset, for meshes that have none */
    void *kstr_cdo;
    uint32_t ragdoll_name;      /* FName "Ragdoll" (number 0) */
} RD;

static int ragdoll_resolve(void)
{
    cls_req q[] = { { "/Script/FortniteGame", "FortPawn" }, { "/Script/Engine", "Character" }, { "/Script/Engine", "Actor" },
        { "/Script/Engine", "SceneComponent" }, { "/Script/Engine", "PrimitiveComponent" }, { "/Script/Engine", "SkeletalMeshComponent" },
        { "/Script/Engine", "SkinnedMeshComponent" }, { "/Script/Engine", "SkeletalMesh" }, { "/Script/Engine", "PhysicsAsset" },
        { "/Script/Engine", "CharacterMovementComponent" }, { "/Script/Engine", "KismetStringLibrary" } };
    find_classes(q, sizeof q / sizeof q[0]);
    RD.fort_pawn = q[0].found; RD.character = q[1].found; RD.actor = q[2].found; RD.scene = q[3].found; RD.prim = q[4].found;
    RD.skc = q[5].found; RD.skinned = q[6].found; RD.skm = q[7].found; RD.physasset = q[8].found; RD.move = q[9].found;
    void *kstr = q[10].found;
    for (int k = 0; k < (int)(sizeof q / sizeof q[0]); k++) if (!q[k].found) { logf_("ragdoll: class %s.%s not found", q[k].pkg, q[k].name); return 0; }
    RD.is_dying = bool_prop(RD.fort_pawn, "bIsDying");
    RD.hidden_for_death = bool_prop(RD.fort_pawn, "bIsHiddenForDeath");
    RD.actor_hidden = bool_prop(RD.actor, "bHidden");
    RD.comp_hidden = bool_prop(RD.scene, "bHiddenInGame");
    RD.comp_visible = bool_prop(RD.scene, "bVisible");
    RD.blend_physics = bool_prop(RD.skc, "bBlendPhysics");
    RD.o_mesh = prop_off(RD.character, "Mesh");
    RD.o_capsule = prop_off(RD.character, "CapsuleComponent");
    RD.o_movement = prop_off(RD.character, "CharacterMovement");
    RD.o_skelmesh = prop_off(RD.skinned, "SkeletalMesh");
    RD.o_pa_override = prop_off(RD.skinned, "PhysicsAssetOverride");
    RD.o_physasset = prop_off(RD.skm, "PhysicsAsset");
    RD.o_bodies = prop_off(RD.physasset, "SkeletalBodySetups");
    RD.o_children = prop_off(RD.scene, "AttachChildren");
    RD.o_velocity = prop_off(RD.move, "Velocity");
    RD.o_controller = prop_off(RD.character, "Controller");
    RD.set_physasset = fn_of(RD.skinned, "SetPhysicsAsset");
    /* the death effect event: the blueprint's override and the native declaration */
    {
        cls_req bq[] = { { "/Game/Athena/PlayerPawn_Athena", "PlayerPawn_Athena_C" } };
        find_classes(bq, 1);
        void *f1 = bq[0].found ? find_func(bq[0].found, "OnDeathPlayEffects") : NULL, *f2 = find_func(RD.fort_pawn, "OnDeathPlayEffects");
        g_nskip = 0;
        if (f1) g_skip_fn[g_nskip++] = f1;
        if (f2 && f2 != f1) g_skip_fn[g_nskip++] = f2;
        logf_("ragdoll: OnDeathPlayEffects %p (PlayerPawn_Athena_C %p) / %p (FortPawn), Controller at %d", f1, bq[0].found, f2, RD.o_controller);
    }
    logf_("ragdoll: bIsDying %d/0x%x, bIsHiddenForDeath %d/0x%x, bHidden %d, bHiddenInGame %d, bVisible %d, bBlendPhysics %d; Mesh %d, Capsule %d, Movement %d, SkeletalMesh %d, PhysicsAsset %d/%d, bodies %d, children %d",
          RD.is_dying.off, RD.is_dying.mask, RD.hidden_for_death.off, RD.hidden_for_death.mask, RD.actor_hidden.off, RD.comp_hidden.off,
          RD.comp_visible.off, RD.blend_physics.off, RD.o_mesh, RD.o_capsule, RD.o_movement, RD.o_skelmesh, RD.o_physasset, RD.o_pa_override,
          RD.o_bodies, RD.o_children);
    if (RD.is_dying.off < 0 || RD.o_mesh < 0 || RD.o_capsule < 0 || RD.o_skelmesh < 0 || RD.o_children < 0) return 0;
    RD.set_hidden_actor = fn_of(RD.actor, "SetActorHiddenInGame");
    RD.destroy = fn_of(RD.actor, "K2_DestroyActor");
    RD.set_sim = fn_of(RD.prim, "SetSimulatePhysics");
    RD.set_all_sim = fn_of(RD.skc, "SetAllBodiesSimulatePhysics");
    RD.set_profile = fn_of(RD.prim, "SetCollisionProfileName");
    RD.set_coll = fn_of(RD.prim, "SetCollisionEnabled");
    RD.wake = fn_of(RD.prim, "WakeAllRigidBodies");
    RD.impulse = fn_of(RD.prim, "AddImpulse");
    RD.set_gravity = fn_of(RD.prim, "SetEnableGravity");
    RD.disable_move = fn_of(RD.move, "DisableMovement");
    RD.stop_move = fn_of(RD.move, "StopMovementImmediately");
    RD.set_vis_comp = fn_of(RD.scene, "SetVisibility");
    RD.set_hidden_comp = fn_of(RD.scene, "SetHiddenInGame");
    RD.set_pawn_vis = fn_of(RD.fort_pawn, "SetPawnVisibility");
    RD.str_to_name = fn_of(kstr, "Conv_StringToName");
    RD.kstr_cdo = find_object_named("Default__KismetStringLibrary");
    logf_("ragdoll: functions SetActorHiddenInGame %p, SetSimulatePhysics %p, SetAllBodiesSimulatePhysics %p, SetCollisionProfileName %p, SetCollisionEnabled %p, AddImpulse %p, DisableMovement %p, SetPawnVisibility %p, Conv_StringToName %p (%p)",
          RD.set_hidden_actor.fn, RD.set_sim.fn, RD.set_all_sim.fn, RD.set_profile.fn, RD.set_coll.fn, RD.impulse.fn, RD.disable_move.fn,
          RD.set_pawn_vis.fn, RD.str_to_name.fn, RD.kstr_cdo);
    if (!RD.set_sim.fn || !RD.set_all_sim.fn || !RD.set_profile.fn || !RD.set_coll.fn) return 0;
    if (RD.str_to_name.fn && RD.kstr_cdo) {
        u8 p[512]; memset(p, 0, sizeof p);
        int oi = parm(RD.str_to_name, "inString"), orv = parm(RD.str_to_name, "ReturnValue");
        if (oi < 0) oi = parm(RD.str_to_name, "InString");
        static wchar_t s[] = L"Ragdoll";
        if (oi >= 0 && orv >= 0) {
            AT(p, oi, wchar_t *) = s; AT(p, oi + 8, int32_t) = 8; AT(p, oi + 12, int32_t) = 8;
            g_pe(RD.kstr_cdo, RD.str_to_name.fn, p);
            RD.ragdoll_name = AT(p, orv, uint32_t);
        }
    }
    char nb[64];
    logf_("ragdoll: collision profile name %u (%s)", RD.ragdoll_name, name_str(RD.ragdoll_name, nb, sizeof nb) ? nb : "?");
    return RD.ragdoll_name != 0;
}

/* ---------------------------------------------------------------- the pawns */
typedef struct {
    void *pawn; int32_t serial;
    int dying, observe, hidden_seen, swapped;
    DWORD t_dead;
    int state;                  /* 0 alive, 1 ragdoll, 2 sinking, 3 hidden (done), 9 observed only */
    void *mesh; void *rezout_seen;
    unsigned logged_mask;
    uint32_t seen_fn[48]; int nseen;
} rpawn;
static rpawn g_p[MAXP]; static int g_np;
static int g_deaths;
static DWORD g_rd_scan;

static const char *cls_name(void *o, char *b, int cap) { return obj_name(o ? AT(o, O_CLASS, void *) : NULL, b, cap); }

/* ---------------------------------------------------------------- ProcessEvent filter on other players' pawns */
/* A copy of the pawn class's vtable (with the RTTI pointer in front of it, vtbl[-1]) whose ProcessEvent entry is
   pe_hook. The hook only compares and forwards: it runs inside the engine, outside the fault guard. Calls on watched
   pawns go to a ring the tick reads (names resolved there). */
typedef struct { void **orig, **clone; process_event_fn pe; } vtswap;   /* pe: the class's own entry (AActor's) */
static vtswap g_vt[8]; static int g_nvt;
static volatile LONG g_skip_count;
#define RING 4096
typedef struct { void *obj, *fn; DWORD t; int skipped; } pe_call;
static pe_call g_ring[RING]; static volatile LONG g_ring_w; static LONG g_ring_r;
#define SKIPPED 32
static void *g_skipped[SKIPPED]; static int g_skipped_w;   /* pawns whose OnDeathPlayEffects was dropped */
static void pe_hook(void *obj, void *fn, void *parms)
{
    if (GetCurrentThreadId() == g_game_tid) {
        int skip = 0;
        for (int k = 0; k < g_nskip; k++) if (fn == g_skip_fn[k]) {
            LONG n = InterlockedIncrement(&g_skip_count);
            skip = g_rd_mode == RD_ON || (g_rd_mode == RD_ALTERNATE && (n & 1) == 0);
            if (skip) { g_skipped[g_skipped_w] = obj; g_skipped_w = (g_skipped_w + 1) % SKIPPED; }
            break;
        }
        LONG w = g_ring_w; pe_call *c = &g_ring[w % RING];
        c->obj = obj; c->fn = fn; c->t = GetTickCount(); c->skipped = skip;
        g_ring_w = w + 1;
        if (skip) return;
    }
    void **vt = *(void ***)obj;
    for (int k = 0; k < g_nvt; k++) if (g_vt[k].clone == vt) { g_vt[k].pe(obj, fn, parms); return; }
    g_pe(obj, fn, parms);
}
/* the effect event of a pawn's class (the blueprint class loads with the match, after ragdoll_resolve) */
static void add_skip_of(void *cls)
{
    void *f = cls ? find_func(cls, "OnDeathPlayEffects") : NULL;
    if (!f) return;
    for (int k = 0; k < g_nskip; k++) if (g_skip_fn[k] == f) return;
    if (g_nskip >= (int)(sizeof g_skip_fn / sizeof g_skip_fn[0])) return;
    char a[128]; logf_("ragdoll: OnDeathPlayEffects of %s: %p", obj_name(cls, a, sizeof a), f);
    g_skip_fn[g_nskip++] = f;
}
static int was_skipped(void *pawn) { for (int k = 0; k < SKIPPED; k++) if (g_skipped[k] == pawn) return 1; return 0; }
static int vt_swap(void *o)
{
    void **vt = AT(o, 0, void **);
    for (int k = 0; k < g_nvt; k++) if (g_vt[k].clone == vt) return 1;
    vtswap *v = NULL;
    for (int k = 0; k < g_nvt; k++) if (g_vt[k].orig == vt) v = &g_vt[k];
    if (!v) {
        if (g_nvt >= (int)(sizeof g_vt / sizeof g_vt[0])) return 0;
        /* actors override ProcessEvent (AActor::ProcessEvent checks the world, then calls UObject's): the entry differs
           from the UObject one the engine layer found */
        void *e = vt[PE_INDEX];
        if (e != (void *)g_pe && e != actor_pe()) { logf_("ragdoll: vtable %p: entry 0x%x %p is not ProcessEvent, left alone", vt, PE_INDEX, e); return 0; }
        void **mem = VirtualAlloc(NULL, (VT_SLOTS + 1) * sizeof(void *), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!mem) return 0;
        int n = 0;
        while (n < VT_SLOTS + 1 && rd(vt - 1 + n, &mem[n], sizeof(void *))) n++;
        if (n < PE_INDEX + 2) { VirtualFree(mem, 0, MEM_RELEASE); return 0; }
        mem[1 + PE_INDEX] = (void *)pe_hook;
        v = &g_vt[g_nvt]; v->orig = vt; v->clone = mem + 1; v->pe = (process_event_fn)e;
        g_nvt++;
        logf_("ragdoll: vtable %p copied (%d entries), ProcessEvent filtered (class entry exe+0x%llx, UObject's %s)", vt, n - 1,
              (unsigned long long)((u8 *)e - g_base), e == (void *)g_pe ? "same" : "differs");
    }
    AT(o, 0, void **) = v->clone;
    return 1;
}

/* the scan for pawns, in slices of 8192 objects per tick (a full pass in one tick held the game thread 10-20 ms) */
static int32_t g_pcur;
static void scan_pawns(void)
{
    int32_t end = g_pcur + 8192; if (end > g_objs->num) end = g_objs->num;
    for (int32_t i = g_pcur; i < end; i++) {
        int32_t fl; void *o = obj_at(i, &fl);
        if (!o || (fl & (EIF_PendingKill | EIF_Unreachable)) || (AT(o, O_FLAGS, uint32_t) & (RF_ClassDefaultObject | RF_ArchetypeObject))) continue;
        if (!is_a(o, RD.fort_pawn)) continue;
        int32_t serial = serial_of(o), known = 0, slot = -1;
        for (int k = 0; k < g_np; k++) {
            if (g_p[k].pawn == o && g_p[k].serial == serial) { known = 1; break; }
            if (slot < 0 && !alive(g_p[k].pawn, g_p[k].serial)) slot = k;
        }
        if (known) continue;
        if (slot < 0) { if (g_np >= MAXP) continue; slot = g_np++; }
        memset(&g_p[slot], 0, sizeof g_p[slot]);
        g_p[slot].pawn = o; g_p[slot].serial = serial;
        g_p[slot].dying = bool_get(o, RD.is_dying);   /* already dead when first seen: leave it to the game */
        if (g_p[slot].dying) { g_p[slot].state = 3; continue; }
        /* other players' pawns (the local one has its controller on this client) */
        if (g_rd_mode != RD_OBSERVE && !(RD.o_controller >= 0 && AT(o, RD.o_controller, void *))) {
            add_skip_of(AT(o, O_CLASS, void *));
            if (g_nskip) g_p[slot].swapped = vt_swap(o);
        }
    }
    g_pcur = end >= g_objs->num ? 0 : end;
}

/* the events the game ran on dying pawns: each function name once per death, with the time from the death */
static void ring_read(DWORD now)
{
    LONG w = g_ring_w;
    if (w - g_ring_r > RING) g_ring_r = w - RING;
    for (; g_ring_r < w; g_ring_r++) {
        pe_call c = g_ring[g_ring_r % RING];
        for (int i = 0; i < g_np; i++) {
            rpawn *p = &g_p[i];
            if (p->pawn != c.obj || !p->dying || c.t + 200 < p->t_dead || c.t > p->t_dead + 5000) continue;
            char nb[128]; obj_name(c.fn, nb, sizeof nb);
            uint32_t h = 2166136261u; for (const char *q = nb; *q; q++) h = (h ^ (u8)*q) * 16777619u;
            int dup = 0; for (int k = 0; k < p->nseen; k++) if (p->seen_fn[k] == h) dup = 1;
            if (dup || p->nseen >= (int)(sizeof p->seen_fn / sizeof p->seen_fn[0])) continue;
            p->seen_fn[p->nseen++] = h;
            logf_("ragdoll: pawn %p event %s at %+ld ms%s", p->pawn, nb, (long)(c.t - p->t_dead), c.skipped ? " (dropped)" : "");
        }
    }
    (void)now;
}

/* the effect-like objects alive right now (classes named like a death or dissolve effect), for the observe log */
static int fx_like(const char *c)
{
    static const char *pat[] = { "ResOut", "Death", "Dissolve", "Elim", "Rez", "GCN_", "Digit", "Ragdoll", "Corpse", "Ghost" };
    for (int i = 0; i < (int)(sizeof pat / sizeof pat[0]); i++) if (strstr(c, pat[i])) return 1;
    return 0;
}
#define MAXFX 512
static void *g_fx0[MAXFX]; static int g_nfx0;
static void fx_snapshot(void)
{
    g_nfx0 = 0;
    for (int32_t i = 0; i < g_objs->num && g_nfx0 < MAXFX; i++) {
        int32_t fl; void *o = obj_at(i, &fl); char c[128];
        if (!o || (fl & (EIF_PendingKill | EIF_Unreachable)) || (AT(o, O_FLAGS, uint32_t) & (RF_ClassDefaultObject | RF_ArchetypeObject))) continue;
        if (fx_like(cls_name(o, c, sizeof c))) g_fx0[g_nfx0++] = o;
    }
}
static void fx_new(const char *when)
{
    char line[1200]; int n = 0, cnt = 0;
    for (int32_t i = 0; i < g_objs->num; i++) {
        int32_t fl; void *o = obj_at(i, &fl); char c[128], nm[128];
        if (!o || (fl & (EIF_PendingKill | EIF_Unreachable)) || (AT(o, O_FLAGS, uint32_t) & (RF_ClassDefaultObject | RF_ArchetypeObject))) continue;
        if (!fx_like(cls_name(o, c, sizeof c))) continue;
        int old = 0; for (int k = 0; k < g_nfx0; k++) if (g_fx0[k] == o) { old = 1; break; }
        if (old) continue;
        cnt++;
        if (n < (int)sizeof line - 200) n += snprintf(line + n, sizeof line - n, " %s(%s)", c, obj_name(o, nm, sizeof nm));
    }
    logf_("ragdoll: %s: %d new effect-like objects:%s", when, cnt, n ? line : " none");
}

static void *mesh_of(void *pawn) { return AT(pawn, RD.o_mesh, void *); }
static void log_mesh(void *pawn, const char *when)
{
    void *mesh = mesh_of(pawn); char a[128], b[128], c[128];
    if (!mesh) { logf_("ragdoll: %s: pawn %p has no mesh", when, pawn); return; }
    void *sk = AT(mesh, RD.o_skelmesh, void *);
    void *pa = sk && RD.o_physasset >= 0 ? AT(sk, RD.o_physasset, void *) : NULL;
    void *pao = RD.o_pa_override >= 0 ? AT(mesh, RD.o_pa_override, void *) : NULL;
    int nb = pa && RD.o_bodies >= 0 ? ((tarray *)((u8 *)pa + RD.o_bodies))->num : -1;
    logf_("ragdoll: %s: pawn %p (%s) mesh %p %s, physics asset %s (%d bodies), override %s; hidden %d, visible %d, actor hidden %d, hidden for death %d",
          when, pawn, cls_name(pawn, a, sizeof a), mesh, obj_name(sk, b, sizeof b), obj_name(pa, c, sizeof c), nb, pao ? "set" : "none",
          bool_get(mesh, RD.comp_hidden), bool_get(mesh, RD.comp_visible), bool_get(pawn, RD.actor_hidden), bool_get(pawn, RD.hidden_for_death));
    tarray *ch = (tarray *)((u8 *)mesh + RD.o_children);
    for (int i = 0; i < ch->num && i < 16; i++) {
        void *k = ch->data[i]; if (!k || !is_a(k, RD.skinned)) continue;
        void *ksk = AT(k, RD.o_skelmesh, void *);
        logf_("ragdoll:   part %p %s mesh %s, hidden %d, visible %d", k, cls_name(k, a, sizeof a), obj_name(ksk, b, sizeof b),
              bool_get(k, RD.comp_hidden), bool_get(k, RD.comp_visible));
    }
}

/* A mesh without a physics asset (imported skins can come without one) gets the default body's: Fortnite's
   characters share one skeleton, so its 19 bodies fit. Looked up once, by name, among the loaded physics assets. */
static void *fallback_physics(void)
{
    if (RD.fallback_tried) return RD.fallback_pa;
    RD.fallback_tried = 1;
    static const char *want[] = { "Fortnite_M_Avg_Player_Physics", "Fortnite_F_Avg_Player_Physics" };
    for (int w = 0; w < 2 && !RD.fallback_pa; w++)
        for (int32_t i = 0; i < g_objs->num && !RD.fallback_pa; i++) {
            int32_t fl; void *o = obj_at(i, &fl); char nm[96];
            if (!o || (fl & (EIF_PendingKill | EIF_Unreachable)) || AT(o, O_CLASS, void *) != RD.physasset) continue;
            if (!strcmp(obj_name(o, nm, sizeof nm), want[w])) RD.fallback_pa = o;
        }
    logf_("ragdoll: fallback physics asset %p", RD.fallback_pa);
    return RD.fallback_pa;
}
static void ensure_physics(void *mesh)
{
    void *sk = AT(mesh, RD.o_skelmesh, void *);
    void *pa = sk && RD.o_physasset >= 0 ? AT(sk, RD.o_physasset, void *) : NULL;
    void *pao = RD.o_pa_override >= 0 ? AT(mesh, RD.o_pa_override, void *) : NULL;
    if (pa || pao || !RD.set_physasset.fn) return;
    void *fb = fallback_physics();
    if (!fb) return;
    u8 pr[64]; memset(pr, 0, sizeof pr);
    int o = parm(RD.set_physasset, "NewPhysicsAsset"), r = parm(RD.set_physasset, "bForceReInit");
    if (o < 0) return;
    AT(pr, o, void *) = fb; if (r >= 0) pr[r] = 1;
    call(mesh, RD.set_physasset, pr);
    char b[96]; logf_("ragdoll: mesh %p (%s) had no physics asset: the default body's set", mesh, obj_name(sk, b, sizeof b));
}

static void show(rpawn *p);
/* the blueprint's dissolve, undone (PlayerPawn_Athena_C) */
static void undo_dissolve(rpawn *p)
{
    void *cls = AT(p->pawn, O_CLASS, void *);
    int o_rez = prop_off(cls, "RezOutFXActor");
    void *rez = o_rez >= 0 ? AT(p->pawn, o_rez, void *) : NULL;
    if (rez) { call_bool(rez, RD.set_hidden_actor, "bNewHidden", 1); call0(rez, RD.destroy); }
    ufn cleanup = fn_of(cls, "CleanupDissolveFX"), restore = fn_of(cls, "RestoreCharacterMats");
    call0(p->pawn, cleanup);
    call0(p->pawn, restore);
    show(p);
    logf_("ragdoll: pawn %p: effect undone (RezOutFXActor %p, CleanupDissolveFX %p, RestoreCharacterMats %p)", p->pawn, rez, cleanup.fn, restore.fn);
}
static void show(rpawn *p)
{
    void *mesh = mesh_of(p->pawn);
    if (bool_get(p->pawn, RD.actor_hidden)) call_bool(p->pawn, RD.set_hidden_actor, "bNewHidden", 0);
    if (mesh && (bool_get(mesh, RD.comp_hidden) || !bool_get(mesh, RD.comp_visible))) {
        call_bool2(mesh, RD.set_hidden_comp, "NewHidden", 0, "bPropagateToChildren", 1);
        call_bool2(mesh, RD.set_vis_comp, "bNewVisibility", 1, "bPropagateToChildren", 1);
    }
}

static void start_ragdoll(rpawn *p)
{
    void *mesh = mesh_of(p->pawn), *cap = AT(p->pawn, RD.o_capsule, void *), *mv = RD.o_movement >= 0 ? AT(p->pawn, RD.o_movement, void *) : NULL;
    if (!mesh) return;
    float vel[3] = { 0, 0, 0 };
    if (mv && RD.o_velocity >= 0) memcpy(vel, (u8 *)mv + RD.o_velocity, sizeof vel);
    undo_dissolve(p);
    if (cap) { u8 pr[512]; memset(pr, 0, sizeof pr); int o = parm(RD.set_coll, "NewType"); if (o >= 0) pr[o] = 0; call(cap, RD.set_coll, pr); }
    if (mv) { call0(mv, RD.stop_move); call0(mv, RD.disable_move); }
    { u8 pr[512]; memset(pr, 0, sizeof pr); int o = parm(RD.set_profile, "InCollisionProfileName"), u = parm(RD.set_profile, "bUpdateOverlaps");
      if (o >= 0) AT(pr, o, uint32_t) = RD.ragdoll_name; if (u >= 0) pr[u] = 0; call(mesh, RD.set_profile, pr); }
    { u8 pr[512]; memset(pr, 0, sizeof pr); int o = parm(RD.set_coll, "NewType"); if (o >= 0) pr[o] = 3; call(mesh, RD.set_coll, pr); }   /* QueryAndPhysics */
    ensure_physics(mesh);
    if (RD.blend_physics.off >= 0) AT(mesh, RD.blend_physics.off, u8) |= RD.blend_physics.mask;
    call_bool(mesh, RD.set_all_sim, "bNewSimulate", 1);
    call_bool(mesh, RD.set_sim, "bSimulate", 1);
    call0(mesh, RD.wake);
    /* keep what the body was doing, and fall back a little */
    if (RD.impulse.fn) {
        u8 pr[512]; memset(pr, 0, sizeof pr);
        int oi = parm(RD.impulse, "Impulse"), ov = parm(RD.impulse, "bVelChange");
        if (oi >= 0) { float *v = (float *)(pr + oi); v[0] = vel[0]; v[1] = vel[1]; v[2] = vel[2] + 120.0f; }
        if (ov >= 0) pr[ov] = 1;
        call(mesh, RD.impulse, pr);
    }
    p->mesh = mesh;
    p->state = 1;
    log_mesh(p->pawn, "ragdoll on");
}

static void sink(rpawn *p)
{
    void *mesh = mesh_of(p->pawn);
    if (mesh) {
        u8 pr[512]; memset(pr, 0, sizeof pr); int o = parm(RD.set_coll, "NewType"); if (o >= 0) pr[o] = 0; call(mesh, RD.set_coll, pr);
        call_bool(mesh, RD.set_gravity, "bGravityEnabled", 1);
        call0(mesh, RD.wake);
    }
    p->state = 2;
}

static void finish(rpawn *p)
{
    void *mesh = mesh_of(p->pawn);
    if (mesh) { call_bool(mesh, RD.set_sim, "bSimulate", 0); call_bool2(mesh, RD.set_hidden_comp, "NewHidden", 1, "bPropagateToChildren", 1); }
    call_bool(p->pawn, RD.set_hidden_actor, "bNewHidden", 1);
    /* the dead pawn is torn off: the server no longer removes it here, and the game's own removal (at the end of the
       dropped effect) does not come */
    if (p->swapped) call0(p->pawn, RD.destroy);
    p->state = 3;
}

static void ragdoll_tick(void)
{
    static int ready, tries; static DWORD next;
    if (g_rd_mode == RD_OFF) return;
    DWORD now = GetTickCount();
    if (!ready) {
        if (now < next) return;
        if (!ragdoll_resolve()) {
            next = now + 10000;
            if (++tries >= 12) { g_rd_mode = RD_OFF; logf_("ragdoll: FAIL: engine types not found: the game's own death effect stays"); }
            return;
        }
        ready = 1; logf_("ragdoll: ready (mode %d)", g_rd_mode);
    }
    scan_pawns(); (void)g_rd_scan;
    for (int i = 0; i < g_np; i++) {
        rpawn *p = &g_p[i];
        if (!p->pawn) continue;
        if (!alive(p->pawn, p->serial)) {
            /* its copy of the vtable stays (others of its class share it); the object is gone */
            if (p->dying) logf_("ragdoll: pawn %p gone %lu ms after its death (state %d)", p->pawn, now - p->t_dead, p->state);
            p->pawn = NULL; continue;
        }
        if (!p->dying) {
            if (!bool_get(p->pawn, RD.is_dying)) continue;
            p->dying = 1; p->t_dead = now; g_deaths++;
            /* a pawn whose effect event was dropped (or that was not filtered: the vtable copy failed) gets the ragdoll */
            p->observe = g_rd_mode == RD_OBSERVE || (g_rd_mode == RD_ALTERNATE && (p->swapped ? !was_skipped(p->pawn) : (g_deaths & 1)));
            log_mesh(p->pawn, p->observe ? "death (observed)" : p->swapped ? "death (effect dropped)" : "death (not filtered)");
            if (p->observe) { fx_snapshot(); p->state = 9; test_on_death(p->pawn, 0); }
            else { start_ragdoll(p); test_on_death(p->pawn, 1); }
            continue;
        }
        DWORD age = now - p->t_dead;
        int hfd = bool_get(p->pawn, RD.hidden_for_death);
        if (hfd && !p->hidden_seen) { p->hidden_seen = 1; logf_("ragdoll: pawn %p hidden for death after %lu ms", p->pawn, age); }
        if (p->state == 9) {
            /* observe: what the game does, at 0.3 s, 1 s and 3 s */
            static const DWORD at[] = { 300, 1000, 3000 };
            for (int k = 0; k < 3; k++) if (age >= at[k] && !(p->logged_mask & (1u << k))) {
                p->logged_mask |= 1u << k;
                char w[48]; snprintf(w, sizeof w, "observed +%lu ms", at[k]);
                log_mesh(p->pawn, w); fx_new(w);
                void *cls = AT(p->pawn, O_CLASS, void *); int o_rez = prop_off(cls, "RezOutFXActor");
                char rn[128]; void *rez = o_rez >= 0 ? AT(p->pawn, o_rez, void *) : NULL;
                logf_("ragdoll:   RezOutFXActor %p %s", rez, cls_name(rez, rn, sizeof rn));
            }
            continue;
        }
        if (p->state == 1) {
            if (!(p->logged_mask & 1) && age >= 1000) { p->logged_mask |= 1; log_mesh(p->pawn, "ragdoll +1 s"); }
            show(p);   /* the server hides dead pawns (bIsHiddenForDeath): the body stays until it sinks */
            if (age >= RAGDOLL_HOLD_MS) sink(p);
        } else if (p->state == 2) {
            show(p);
            if (age >= RAGDOLL_HOLD_MS + RAGDOLL_SINK_MS) { finish(p); logf_("ragdoll: pawn %p done after %lu ms", p->pawn, age); }
        }
    }
    ring_read(now);
}

static void ragdoll_init(void)
{
    char e[32];
    DWORD n = GetEnvironmentVariableA("FNRAGDOLL", e, sizeof e);
    if (n && n < sizeof e) {
        if (!strcmp(e, "0") || !_stricmp(e, "off")) g_rd_mode = RD_OFF;
        else if (!_stricmp(e, "observe")) g_rd_mode = RD_OBSERVE;
        else if (!_stricmp(e, "alternate")) g_rd_mode = RD_ALTERNATE;
    }
    logf_("ragdoll: mode %s", g_rd_mode == RD_OFF ? "off" : g_rd_mode == RD_OBSERVE ? "observe" : g_rd_mode == RD_ALTERNATE ? "alternate" : "on");
}
