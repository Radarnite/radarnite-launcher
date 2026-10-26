/*
 * fnbody.c - the body sliders module of fnmods.dll (#included by fnmods.c, which owns the engine access, the game-thread
 * tick and the fault guard): the shape side of the body sliders of fn-port outfits in Fortnite 14.30.
 *
 * fn-port gives every ported outfit one FortCosmeticFloatSliderVariant style per body region (the class of the build's
 * own "Corruption" slider): the game draws it as a slider in its "Edit style" screen, keeps the value in the profile and
 * writes it to the material parameter FNBody_<Region> of the outfit's material instances. fn-port also bakes a morph
 * target FNBody_<Region> into each part mesh. This module is the bridge between the two:
 *   - every 250 ms, and as soon as a known component dies or changes mesh (the locker re-applies the style), it lists the
 *     live USkeletalMeshComponents (GUObjectArray) whose mesh has FNBody_* morph targets or whose material instances
 *     (OverrideMaterials, UMaterialInstanceDynamic::ScalarParameterValues) carry FNBody_* parameters; components are
 *     grouped by the root of their attachment (one character);
 *   - every tick, for each character, it calls USkeletalMeshComponent::SetMorphTarget(FNBody_<Region>, weight) when the
 *     engine's weight (GetMorphTarget) differs on its components with those morphs (weight = (s - 1) / (RefScale - 1),
 *     s = slider % / 100, RefScale = 10: the scale fn-port baked the deltas at), and Height scales the part meshes'
 *     parent component (SetRelativeScale3D, from the scale it had).
 * Property offsets come from the engine's own reflection, by name. mods.json "bodySliders" turns it off.
 * Not verified in a game session: an fn-port outfit with sliders worn by the test profile (the log says what it finds).
 */

/* ---------------------------------------------------------------- the sliders (fn-port tools/fn-port/BodySliders.cs) */
#define NREG 12
static const char *REG_NAME[NREG] = { "Height", "Head", "Neck", "Shoulders", "Chest", "Waist", "Glutes", "Arms", "Forearms", "Hands", "Thighs", "Calves" };
static const float REG_MIN[NREG] = { 50, 25, 10, 10, 0, 10, 0, 10, 10, 25, 10, 10 };
static const float REG_MAX[NREG] = { 200, 400, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 400, 1000, 1000 };
#define REF_SCALE 10.0f
#define PREFIX "FNBody_"

/* ---------------------------------------------------------------- reflection we need */
static struct {
    void *skc, *mid;                                      /* SkeletalMeshComponent, MaterialInstanceDynamic */
    int o_mesh, o_override, o_attach, o_scale, o_morphs, o_scalars;
    int sp_size, sp_name, sp_value;                       /* FScalarParameterValue: stride, ParameterInfo.Name, ParameterValue */
    void *f_setmorph; int fm_name, fm_value, fm_remove, fm_size;
    void *f_setscale; int fs_scale, fs_size;
} R;

static int resolve(void)
{
    type_req q[] = { { "Class", "SkeletalMeshComponent" }, { "Class", "MaterialInstanceDynamic" }, { "Class", "SkinnedMeshComponent" },
        { "Class", "MeshComponent" }, { "Class", "SceneComponent" }, { "Class", "SkeletalMesh" }, { "Class", "MaterialInstance" },
        { "ScriptStruct", "ScalarParameterValue" }, { "ScriptStruct", "MaterialParameterInfo" } };
    find_types(q, sizeof q / sizeof q[0]);
    R.skc = q[0].found; R.mid = q[1].found;
    void *skinned = q[2].found, *meshc = q[3].found, *scene = q[4].found, *skm = q[5].found, *mi = q[6].found, *spv = q[7].found, *mpi = q[8].found;
    logf_("types: SkeletalMeshComponent %p, MaterialInstanceDynamic %p, SkinnedMeshComponent %p, MeshComponent %p, SceneComponent %p, SkeletalMesh %p, MaterialInstance %p, ScalarParameterValue %p, MaterialParameterInfo %p",
          R.skc, R.mid, skinned, meshc, scene, skm, mi, spv, mpi);
    /* 14.30 did not give the two structs by name: take them from the properties that hold them */
    if (mi && (!spv || !mpi)) {
        void *f = find_prop(mi, "ScalarParameterValues"), *fi;
        if (!spv && f) spv = array_struct_of(f);
        if (!mpi && spv && (fi = find_prop(spv, "ParameterInfo"))) mpi = struct_of(fi);
        static int diag_done;
        if (!spv && !diag_done++) {
            dump_prop("ScalarParameterValues", f);
            char fl[512] = ""; int nf = 0;
            for (void *x = AT(mi, S_CHILDPROPS, void *); x && nf < 16; x = AT(x, FF_NEXT, void *), nf++) {
                char n[64]; if (!name_str(AT(x, FF_NAME, uint32_t), n, sizeof n)) snprintf(n, sizeof n, "?");
                strncat(fl, n, sizeof fl - strlen(fl) - 2); strncat(fl, " ", sizeof fl - strlen(fl) - 1);
            }
            logf_("MaterialInstance properties (first %d): %s", nf, fl);
            int shown = 0;
            for (int32_t i = 0; i < g_objs->num && shown < 6; i++) {
                void *o = obj_at(i, NULL); char n[128], c[128], w[128];
                if (!o || !name_str(AT(o, O_NAME, uint32_t), n, sizeof n) || (strcmp(n, "ScalarParameterValue") && strcmp(n, "ScriptStruct"))) continue;
                shown++;
                logf_("object %d %s: class %s, outer %s", i, n, obj_name(AT(o, O_CLASS, void *), c, sizeof c), obj_name(AT(o, O_OUTER, void *), w, sizeof w));
            }
            logf_("%d objects", g_objs->num);
            objs_diag();
        }
        char a[128], b[128], c[128];
        logf_("structs from the properties: ScalarParameterValue %p (%s, in %s), MaterialParameterInfo %p (%s)", spv, obj_name(spv, a, sizeof a),
              spv ? obj_name(AT(spv, O_OUTER, void *), b, sizeof b) : "-", mpi, obj_name(mpi, c, sizeof c));
    }
    if (!R.skc || !R.mid || !skinned || !meshc || !scene || !skm || !mi || !spv || !mpi) return 0;
    R.o_mesh = prop_off(skinned, "SkeletalMesh");
    R.o_override = prop_off(meshc, "OverrideMaterials");
    R.o_attach = prop_off(scene, "AttachParent");
    R.o_scale = prop_off(scene, "RelativeScale3D");
    R.o_morphs = prop_off(skm, "MorphTargets");
    R.o_scalars = prop_off(mi, "ScalarParameterValues");
    int pinfo = prop_off(spv, "ParameterInfo"), pname = prop_off(mpi, "Name");
    R.sp_value = prop_off(spv, "ParameterValue");
    int align = AT(spv, S_MINALIGN, int32_t); if (align < 1) align = 1;
    R.sp_size = (AT(spv, S_PROPSIZE, int32_t) + align - 1) / align * align;
    R.sp_name = pinfo >= 0 && pname >= 0 ? pinfo + pname : -1;
    logf_("offsets: SkeletalMesh %d, OverrideMaterials %d, AttachParent %d, RelativeScale3D %d, MorphTargets %d, ScalarParameterValues %d; FScalarParameterValue size %d name %d value %d",
          R.o_mesh, R.o_override, R.o_attach, R.o_scale, R.o_morphs, R.o_scalars, R.sp_size, R.sp_name, R.sp_value);
    if (R.o_mesh < 0 || R.o_override < 0 || R.o_attach < 0 || R.o_scale < 0 || R.o_morphs < 0 || R.o_scalars < 0 || R.sp_name < 0 || R.sp_value < 0 || R.sp_size <= 0) return 0;
    R.f_setmorph = find_func(R.skc, "SetMorphTarget");
    R.f_setscale = find_func(scene, "SetRelativeScale3D");
    if (!R.f_setmorph || !R.f_setscale) { logf_("functions: SetMorphTarget %p, SetRelativeScale3D %p", R.f_setmorph, R.f_setscale); return 0; }
    R.fm_name = prop_off(R.f_setmorph, "MorphTargetName"); R.fm_value = prop_off(R.f_setmorph, "Value"); R.fm_remove = prop_off(R.f_setmorph, "bRemoveZeroWeight");
    R.fm_size = AT(R.f_setmorph, S_PROPSIZE, int32_t);
    R.fs_scale = prop_off(R.f_setscale, "NewScale3D"); R.fs_size = AT(R.f_setscale, S_PROPSIZE, int32_t);
    logf_("functions: SetMorphTarget %p (MorphTargetName %d, Value %d, bRemoveZeroWeight %d, %d bytes), SetRelativeScale3D %p (NewScale3D %d, %d bytes)",
          R.f_setmorph, R.fm_name, R.fm_value, R.fm_remove, R.fm_size, R.f_setscale, R.fs_scale, R.fs_size);
    return R.fm_name >= 0 && R.fm_value >= 0 && R.fs_scale >= 0 && R.fm_size > 0 && R.fm_size <= 256 && R.fs_size > 0 && R.fs_size <= 256;
}

/* ---------------------------------------------------------------- per-frame work */
typedef struct { void **data; int32_t num, max; } tarray;
static int region_of(uint32_t idx)
{
    char b[64];
    if (!name_str(idx, b, sizeof b) || strncmp(b, PREFIX, sizeof PREFIX - 1)) return -1;
    for (int r = 0; r < NREG; r++) if (!strcmp(b + sizeof PREFIX - 1, REG_NAME[r])) return r;
    return -1;
}

static DWORD g_ready_at;
static float g_test[NREG]; static int g_testing;   /* test mode, below */
#define MAXC 256
typedef struct { void *comp; int32_t serial; void *root; void *mesh; uint32_t morph_mask; uint32_t morph_name[NREG]; unsigned logged; float lastv[NREG]; } cand;
static cand g_c[MAXC]; static int g_nc; static int g_rescan;   /* a candidate died or changed mesh: scan again this tick */
typedef struct { void *comp; int32_t serial; float sx, sy, sz; float applied; } scaled;
static scaled g_s[64]; static int g_ns;

static void *root_of(void *c)
{
    void *r = c;
    for (int d = 0; d < 32; d++) { void *p = AT(r, R.o_attach, void *); if (!p) break; r = p; }
    return r;
}
/* FNBody_* parameters on a component's dynamic material instances: values by region (NAN = none) */
static void params_of(void *comp, float *val)
{
    tarray *ov = (tarray *)((u8 *)comp + R.o_override);
    for (int i = 0; i < ov->num && i < 64; i++) {
        void *m = ov->data[i];
        if (!m || !is_a(m, R.mid)) continue;
        tarray *sp = (tarray *)((u8 *)m + R.o_scalars);
        for (int k = 0; k < sp->num && k < 512; k++) {
            u8 *e = (u8 *)sp->data + (size_t)k * R.sp_size;
            int r = region_of(AT(e, R.sp_name, uint32_t));
            if (r >= 0) val[r] = AT(e, R.sp_value, float);
        }
    }
}
static uint32_t morphs_of(void *comp, uint32_t *names)
{
    void *mesh = AT(comp, R.o_mesh, void *); uint32_t mask = 0;
    if (!mesh) return 0;
    tarray *mt = (tarray *)((u8 *)mesh + R.o_morphs);
    for (int i = 0; i < mt->num && i < 4096; i++) {
        void *m = mt->data[i]; if (!m) continue;
        uint32_t n = AT(m, O_NAME, uint32_t); int r = region_of(n);
        if (r >= 0 && AT(m, O_NAME + 4, uint32_t) == 0) { mask |= 1u << r; names[r] = n; }
    }
    return mask;
}

static DWORD g_scan_max; static int g_scans;
/* The scan for candidates goes over every object (about 300,000 in a match): done in one tick it held the game thread
   10-20 ms every 250 ms. It runs in slices of COLLECT_SLICE objects per tick instead (about a millisecond each)
   into g_new; the finished list replaces g_c, which apply() keeps using meanwhile. */
#define COLLECT_SLICE 8192
static cand g_new[MAXC]; static int g_nnew; static int32_t g_cur = -1; static DWORD g_pass_us;
static void collect_begin(void) { g_cur = 0; g_nnew = 0; g_pass_us = 0; }
static void collect_step(void)
{
    LARGE_INTEGER f0, t0, t1; QueryPerformanceFrequency(&f0); QueryPerformanceCounter(&t0);
    cand *old = g_c; int nold = g_nc;
    int32_t end = g_cur + COLLECT_SLICE; if (end > g_objs->num) end = g_objs->num;
    for (int32_t i = g_cur; i < end && g_nnew < MAXC; i++) {
        int32_t fl; void *o = obj_at(i, &fl);
        if (!o || (fl & (EIF_PendingKill | EIF_Unreachable)) || (AT(o, O_FLAGS, uint32_t) & (RF_ClassDefaultObject | RF_ArchetypeObject | RF_NeedLoad))) continue;
        if (!is_a(o, R.skc)) continue;
        cand c = { 0 }; c.comp = o; c.mesh = AT(o, R.o_mesh, void *);
        for (int r = 0; r < NREG; r++) c.lastv[r] = NAN;
        c.morph_mask = morphs_of(o, c.morph_name);
        float v[NREG]; for (int r = 0; r < NREG; r++) v[r] = NAN;
        params_of(o, v);
        int any = c.morph_mask != 0; for (int r = 0; r < NREG && !any; r++) if (v[r] == v[r]) any = 1;
        if (!any) continue;
        c.serial = serial_of(o); c.root = root_of(o);
        int seen = 0;
        for (int k = 0; k < nold; k++) if (old[k].comp == o && old[k].serial == c.serial) { c.logged = old[k].logged; memcpy(c.lastv, old[k].lastv, sizeof c.lastv); seen = 1; }
        if (!seen) {
            char b[300]; int n = 0;
            for (int r = 0; r < NREG; r++) if (v[r] == v[r]) n += snprintf(b + n, sizeof b - n, " %s=%.0f", REG_NAME[r], v[r]);
            char mn[128]; void *mesh = AT(o, R.o_mesh, void *);
            logf_("component %p (mesh %s): morphs 0x%x, parameters:%s", o, obj_name(mesh, mn, sizeof mn), c.morph_mask, n ? b : " none");
        }
        g_new[g_nnew++] = c;
    }
    g_cur = end;
    QueryPerformanceCounter(&t1);
    DWORD step_us = (DWORD)((t1.QuadPart - t0.QuadPart) * 1000000 / f0.QuadPart);
    g_pass_us += step_us;
    if (step_us > g_scan_max) { g_scan_max = step_us; logf_("scan slice: %lu us (max so far)", step_us); }
    if (g_cur < g_objs->num && g_nnew < MAXC) return;
    /* the pass is complete */
    memcpy(g_c, g_new, sizeof(cand) * (size_t)g_nnew); g_nc = g_nnew; g_cur = -1;
    static int listed;
    if (g_testing && !g_nc && !listed && GetTickCount() - g_ready_at > 20000) {
        listed = 1; int shown = 0;
        for (int32_t i = 0; i < g_objs->num && shown < 24; i++) {
            int32_t fl; void *o = obj_at(i, &fl);
            if (!o || (fl & (EIF_PendingKill | EIF_Unreachable)) || (AT(o, O_FLAGS, uint32_t) & (RF_ClassDefaultObject | RF_ArchetypeObject)) || !is_a(o, R.skc)) continue;
            void *mesh = AT(o, R.o_mesh, void *); char mn[128]; int nm = 0; char first[200] = "";
            if (mesh) {
                tarray *mt = (tarray *)((u8 *)mesh + R.o_morphs); nm = mt->num;
                for (int k = 0, w = 0; k < mt->num && k < 4 && mt->data[k]; k++) { char t[64]; w += snprintf(first + w, sizeof first - w, " %s", obj_name(mt->data[k], t, sizeof t)); }
            }
            tarray *ov = (tarray *)((u8 *)o + R.o_override);
            logf_("test: skeletal mesh component %p, mesh %s, %d morphs%s, %d override materials", o, obj_name(mesh, mn, sizeof mn), nm, first, ov->num);
            shown++;
        }
    }
    g_scans++;
    if (g_scans == 1 || g_scans % 2400 == 0) logf_("scan %d: %lu us in slices over %d objects (slowest slice %lu us)", g_scans, g_pass_us, g_objs->num, g_scan_max);
    static int last = -1;
    if (g_nc != last) { logf_("%d skeletal mesh components with body slider morphs or parameters", g_nc); last = g_nc; }
}

/* slider value (%) from the material parameter: the client writes the slider's value (MinParamValue..MaxParamValue) */
static float scale_of(int r, float v)
{
    if (v < REG_MIN[r]) v = REG_MIN[r];
    if (v > REG_MAX[r]) v = REG_MAX[r];
    return v / 100.0f;
}

static void set_morph(void *comp, uint32_t name, float w)
{
    u8 parms[256]; memset(parms, 0, sizeof parms);
    AT(parms, R.fm_name, uint32_t) = name; AT(parms, R.fm_name + 4, uint32_t) = 0;
    AT(parms, R.fm_value, float) = w;
    if (R.fm_remove >= 0) AT(parms, R.fm_remove, u8) = 1;
    g_pe(comp, R.f_setmorph, parms);
}
static void set_height(void *target, float h)
{
    scaled *s = NULL;
    for (int i = 0; i < g_ns; i++) if (g_s[i].comp == target && alive(target, g_s[i].serial)) { s = &g_s[i]; break; }
    if (!s) {
        if (h == 1.0f) return;   /* never touched and nothing to do */
        int slot = -1;
        for (int i = 0; i < g_ns; i++) if (!alive(g_s[i].comp, g_s[i].serial)) { slot = i; break; }
        if (slot < 0) { if (g_ns >= 64) return; slot = g_ns++; }
        s = &g_s[slot]; s->comp = target; s->serial = serial_of(target);
        float *sc = (float *)((u8 *)target + R.o_scale); s->sx = sc[0]; s->sy = sc[1]; s->sz = sc[2]; s->applied = 1.0f;
    }
    /* the game put its own scale back (a re-applied style): that is the base again */
    float *cs = (float *)((u8 *)target + R.o_scale);
    if (fabsf(cs[0] - s->sx * s->applied) > 1e-3f) { s->sx = cs[0]; s->sy = cs[1]; s->sz = cs[2]; s->applied = 1.0f; }
    if (s->applied == h) return;
    u8 parms[256]; memset(parms, 0, sizeof parms);
    float *v = (float *)(parms + R.fs_scale); v[0] = s->sx * h; v[1] = s->sy * h; v[2] = s->sz * h;
    g_pe(target, R.f_setscale, parms);
    s->applied = h;
}

/* ---------------------------------------------------------------- test mode (FNBODY_TEST=Head:150,Arms:200,...)
   Without the game's UI: those slider values (%) win over the material parameters, every weight set is read back with
   GetMorphTarget and logged, and the engine takes a screenshot (HighResShot) before and after. */
static void test_parse(void)
{
    char e[256]; for (int r = 0; r < NREG; r++) g_test[r] = NAN;
    DWORD n = GetEnvironmentVariableA("FNBODY_TEST", e, sizeof e);
    if (!n || n >= sizeof e) return;
    for (char *t = strtok(e, ","); t; t = strtok(NULL, ",")) {
        char *c = strchr(t, ':'); if (!c) continue; *c = 0;
        for (int r = 0; r < NREG; r++) if (!_stricmp(t, REG_NAME[r])) { g_test[r] = (float)atof(c + 1); g_testing = 1; }
    }
    if (g_testing) {
        char b[300]; int k = 0;
        for (int r = 0; r < NREG; r++) if (g_test[r] == g_test[r]) k += snprintf(b + k, sizeof b - k, " %s=%.0f", REG_NAME[r], g_test[r]);
        logf_("test mode:%s", b);
    }
}
static void *g_f_getmorph; static int g_fg_name = -1, g_fg_ret = -1;
static float get_morph(void *comp, uint32_t name)
{
    if (!g_f_getmorph) {
        g_f_getmorph = find_func(R.skc, "GetMorphTarget");
        if (g_f_getmorph) { g_fg_name = prop_off(g_f_getmorph, "MorphTargetName"); g_fg_ret = prop_off(g_f_getmorph, "ReturnValue"); }
        if (!g_f_getmorph || g_fg_name < 0 || g_fg_ret < 0) { g_f_getmorph = (void *)1; logf_("GetMorphTarget not found: weights are set every tick"); }
    }
    if (g_f_getmorph == (void *)1) return NAN;
    u8 parms[64]; memset(parms, 0, sizeof parms);
    AT(parms, g_fg_name, uint32_t) = name;
    g_pe(comp, g_f_getmorph, parms);
    return AT(parms, g_fg_ret, float);
}

/* test steps, on the game thread: 0 wait for a character with FNBody morphs, 1 screenshot before, 2 apply (from now on),
   3 read back, 4 screenshot after, 5 done */
static int g_tstep; static DWORD g_tnext;
static void test_step(void)
{
    if (!g_testing || g_tstep >= 5 || GetTickCount() < g_tnext) return;
    cand *c = NULL; for (int i = 0; i < g_nc && !c; i++) if (g_c[i].comp && g_c[i].morph_mask) c = &g_c[i];
    if (!c) return;
    switch (g_tstep) {
    case 0: logf_("test: character %p, component %p, morphs 0x%x", c->root, c->comp, c->morph_mask); g_tnext = GetTickCount() + 3000; break;
    case 1: console(c->comp, L"HighResShot 1"); g_tnext = GetTickCount() + 4000; break;
    case 2: logf_("test: applying the test values"); g_tnext = GetTickCount() + 2000; break;
    case 3:
        for (int i = 0; i < g_nc; i++) {
            cand *k = &g_c[i]; if (!k->comp || !k->morph_mask) continue;
            char b[600]; int n = 0;
            for (int r = 1; r < NREG; r++) if (k->morph_mask & (1u << r)) n += snprintf(b + n, sizeof b - n, " %s=%.4f", REG_NAME[r], get_morph(k->comp, k->morph_name[r]));
            float *sc = (float *)((u8 *)k->comp + R.o_scale);
            void *p = AT(k->comp, R.o_attach, void *); float *ps = p ? (float *)((u8 *)p + R.o_scale) : NULL;
            logf_("test: component %p weights read back:%s; scale %.3f, parent scale %.3f", k->comp, b, sc[0], ps ? ps[0] : 0.0f);
        }
        g_tnext = GetTickCount() + 1000; break;
    case 4: console(c->comp, L"HighResShot 1"); logf_("test: done"); break;
    }
    g_tstep++;
}

static int g_logged_apply;
static void apply(void)
{
    for (int i = 0; i < g_nc; i++) {
        cand *c = &g_c[i];
        if (!c->comp) continue;
        if (!alive(c->comp, c->serial) || AT(c->comp, R.o_mesh, void *) != c->mesh) { c->comp = NULL; g_rescan = 1; continue; }
    }
    /* per character (attachment root): the parameters any of its components carries */
    for (int i = 0; i < g_nc; i++) {
        if (!g_c[i].comp) continue;
        void *root = g_c[i].root; int first = 1;
        for (int k = 0; k < i; k++) if (g_c[k].comp && g_c[k].root == root) { first = 0; break; }
        if (!first) continue;
        float v[NREG]; for (int r = 0; r < NREG; r++) v[r] = NAN;
        for (int k = i; k < g_nc; k++) if (g_c[k].comp && g_c[k].root == root) params_of(g_c[k].comp, v);
        if (g_testing) { if (g_tstep < 3) continue; for (int r = 0; r < NREG; r++) if (g_test[r] == g_test[r]) v[r] = g_test[r]; }
        for (int k = i; k < g_nc; k++) {
            cand *c = &g_c[k];
            if (!c->comp || c->root != root || !c->morph_mask) continue;
            for (int r = 1; r < NREG; r++) {
                if (!(c->morph_mask & (1u << r)) || v[r] != v[r]) continue;
                float w = (scale_of(r, v[r]) - 1.0f) / (REF_SCALE - 1.0f);
                /* only when the engine's weight differs: a re-applied style or a new mesh resets it, a still slider costs a read */
                float cur = get_morph(c->comp, c->morph_name[r]);
                if (cur != cur || fabsf(cur - w) > 1e-4f) set_morph(c->comp, c->morph_name[r], w);
                if (c->lastv[r] == c->lastv[r] && c->lastv[r] != v[r] && g_logged_apply < 400) {
                    g_logged_apply++;
                    logf_("%s %.0f -> %.0f on %p: weight %.4f set in the tick that read it (engine had %.4f)", REG_NAME[r], c->lastv[r], v[r], c->comp, w, cur);
                }
                c->lastv[r] = v[r];
            }
            if (v[0] == v[0]) {
                /* Height: the parent the part meshes copy their pose from (the character's base mesh), else the part */
                void *p = AT(c->comp, R.o_attach, void *);
                void *target = p && is_a(p, R.skc) ? p : c->comp;
                set_height(target, scale_of(0, v[0]));
            }
            if (g_logged_apply < 400) {
                /* one line per component whenever its character's values change */
                char b[400]; int n = 0;
                for (int r = 0; r < NREG; r++) if (v[r] == v[r]) n += snprintf(b + n, sizeof b - n, " %s=%.1f", REG_NAME[r], v[r]);
                if (!n) snprintf(b, sizeof b, " no slider values yet");
                unsigned h = 2166136261u; for (const char *t = b; *t; t++) h = (h ^ (u8)*t) * 16777619u;
                h ^= (unsigned)(uintptr_t)c->comp;
                if (h != c->logged) { c->logged = h; g_logged_apply++; logf_("character %p: component %p (morphs 0x%x):%s", root, c->comp, c->morph_mask, b); }
            }
        }
    }
}

/* ---------------------------------------------------------------- the module's tick (game thread) */
static int g_ready;
static DWORD g_last_collect;

static int g_body_off;
static void body_init(void) { test_parse(); }
static void body_tick(void)
{
    if (g_body_off) return;
    if (!g_ready) {
        /* the lobby is still loading the first seconds: try again every 10 s for 2 minutes */
        static int tries; static DWORD next;
        if (GetTickCount() < next) return;
        if (!resolve()) {
            next = GetTickCount() + 10000;
            if (++tries >= 12) { g_body_off = 1; logf_("body: FAIL: engine types not found: body sliders disabled"); }
            else logf_("body: engine types not found yet (try %d of 12)", tries);
            return;
        }
        g_ready = 1; g_ready_at = GetTickCount(); logf_("body: ready");
    }
    DWORD now = GetTickCount();
    /* a full scan every 250 ms, and right away when a known component died or changed mesh (a re-applied style); in
       slices, see collect_step */
    if (g_cur < 0 && (g_rescan || now - g_last_collect >= 250)) { g_rescan = 0; collect_begin(); g_last_collect = now; }
    if (g_cur >= 0) collect_step();
    apply();
    test_step();
}
