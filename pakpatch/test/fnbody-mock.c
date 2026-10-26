/*
 * fnbody-mock.exe - runs fnmods.dll (its body sliders module) against a fake engine built in this process: the GUObjectArray pattern, the
 * ProcessEvent string reference, an FNamePool, classes / properties / functions with UE 4.26's layout, a character
 * (base mesh + body part with FNBody_* morph targets and a dynamic material instance carrying FNBody_* parameters)
 * and an "UnrealWindow" pumping messages. It checks that the DLL finds everything, calls SetMorphTarget with
 * (s - 1) / 9 and scales the base mesh for Height once, on the window's thread, with a second copy of the DLL loaded under
 * another name staying idle. It cannot check the real game's layout.
 *
 * Build: zig cc -target x86_64-windows-gnu -O1 -o fnbody-mock.exe fnbody-mock.c -luser32
 * Run:   fnbody-mock.exe <folder with fnmods.dll> [--plain]   (exit 0 = pass; names XOR-encoded as in 14.30 unless --plain)
 */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

typedef unsigned char u8;
#define AT(p, off, T) (*(T *)((u8 *)(p) + (off)))

/* ---- name pool (in .data, found by the DLL's scan) */
static struct { uint64_t lock; uint32_t cur_block, cursor; u8 *blocks[8192]; } g_pool = { 0, 0, 0, { 0 } };
static u8 g_block0[1 << 17];
static void *volatile g_pool_escape;   /* only the DLL reads the pool: main stores its address here so its stores stay */
static uint32_t g_cursor;
/* Fortnite 14.30 keeps the name strings XOR-encoded: byte i ^ k_i, k_0 = 0xE0, k_{i+1} = k_i + 7 + i (--plain: as is) */
static int g_encode = 1;
static void encode(u8 *out, const char *s, int len)
{
    u8 k = 0xE0;
    for (int i = 0; i < len; i++) { out[i] = (u8)s[i] ^ (g_encode ? k : 0); k = (u8)(k + 7 + i); }
}
static uint32_t name(const char *s)
{
    u8 enc[256]; int len = (int)strlen(s); encode(enc, s, len);
    for (uint32_t off = 0; off < g_cursor; ) {   /* existing */
        uint16_t h = *(uint16_t *)(g_block0 + off); int l = h >> 6;
        if (l == len && !memcmp(g_block0 + off + 2, enc, len)) return off / 2;
        off += (2 + l + 1) & ~1u;
    }
    uint32_t at = g_cursor;
    *(uint16_t *)(g_block0 + at) = (uint16_t)(len << 6);
    memcpy(g_block0 + at + 2, enc, len);
    g_cursor += (2 + len + 1) & ~1u;
    g_pool.cursor = g_cursor;
    return at / 2;
}

/* ---- objects */
typedef struct { void *obj; int32_t flags, cluster, serial; int32_t pad; } item;   /* 24 bytes */
static item g_items[4096];
static item *g_chunks[1] = { g_items };
static struct { item **objects; void *pre; int32_t max, num, maxchunks, numchunks; } g_array = { g_chunks, 0, 4096, 0, 1, 1 };
static void *g_vtable[0x100];
static u8 *obj(const char *n, void *cls, void *outer)
{
    u8 *o = calloc(1, 0x400);
    AT(o, 0, void *) = g_vtable; AT(o, 0x18, uint32_t) = name(n); AT(o, 0x10, void *) = cls; AT(o, 0x20, void *) = outer;
    int32_t i = g_array.num++; AT(o, 0x0C, int32_t) = i;
    g_items[i].obj = o; g_items[i].serial = 7 + i;
    return o;
}
static u8 *g_classclass, *g_structclass, *g_engine;
static u8 *type(const char *n, u8 *super, int script_struct)
{
    u8 *t = obj(n, script_struct ? g_structclass : g_classclass, g_engine);
    AT(t, 0x40, void *) = super;
    return t;
}
static u8 *prop(u8 *st, const char *n, int32_t off)
{
    u8 *f = calloc(1, 0x80);
    AT(f, 0x28, uint32_t) = name(n); AT(f, 0x4C, int32_t) = off;
    AT(f, 0x20, void *) = AT(st, 0x50, void *); AT(st, 0x50, void *) = f;
    return f;
}
static u8 *func(u8 *cls, const char *n, int32_t size)
{
    u8 *f = obj(n, g_classclass, cls);
    AT(f, 0x58, int32_t) = size;
    AT(f, 0x28, void *) = AT(cls, 0x48, void *); AT(cls, 0x48, void *) = f;
    return f;
}

/* ---- ProcessEvent: the reference the DLL follows, and the fake itself */
static const wchar_t PE_STR[] = L"UMeshNetworkComponent::ProcessEvent: Invalid mesh network node type: %s";
/* read through a volatile pointer so the mock's own code holds no lea to the string (the DLL takes the first one) */
static const wchar_t *volatile PE_PTR = PE_STR;
static u8 g_code[64];
static u8 *g_setmorph, *g_setscale, *g_getmorph;
/* the engine's weights (GetMorphTarget); main clears them once, as a re-applied style does, and times the repair */
static uint32_t g_wname[32]; static float g_wval[32]; static int g_nw; static DWORD g_reset_at, g_fixed_at;
static int g_morph_calls, g_scale_calls; static float g_arms = -1, g_scale = -1; static DWORD g_main_tid, g_bad_thread;
static void mock_pe(void *o, void *fn, void *parms)
{
    if (GetCurrentThreadId() != g_main_tid) g_bad_thread++;
    char b[64] = { 0 };
    if (fn == g_setmorph) {
        uint32_t n = AT(parms, 0, uint32_t); uint16_t h = *(uint16_t *)(g_block0 + 2 * n);
        int l = h >> 6; u8 k = 0xE0; for (int i = 0; i < l && i < 63; i++) { b[i] = (char)(g_block0[2 * n + 2 + i] ^ (g_encode ? k : 0)); k = (u8)(k + 7 + i); }
        g_morph_calls++;
        if (!strcmp(b, "FNBody_Arms")) { g_arms = AT(parms, 8, float); if (g_reset_at && !g_fixed_at) g_fixed_at = GetTickCount(); }
        int k2 = 0; while (k2 < g_nw && g_wname[k2] != n) k2++;
        if (k2 == g_nw && g_nw < 32) g_wname[g_nw++] = n;
        if (k2 < 32) g_wval[k2] = AT(parms, 8, float);
        if (g_morph_calls <= 3) printf("SetMorphTarget(%p, %s, %.4f, %d)\n", o, b, AT(parms, 8, float), AT(parms, 12, u8));
    } else if (fn == g_getmorph) {
        uint32_t n = AT(parms, 0, uint32_t); float v = 0;
        for (int k2 = 0; k2 < g_nw; k2++) if (g_wname[k2] == n) v = g_wval[k2];
        AT(parms, 8, float) = v;
    } else if (fn == g_setscale) {
        g_scale_calls++; g_scale = AT(parms, 0, float);
        float *s = (float *)((u8 *)o + 0x108); s[0] = AT(parms, 0, float); s[1] = AT(parms, 4, float); s[2] = AT(parms, 8, float);
        printf("SetRelativeScale3D(%p, %.3f %.3f %.3f)\n", o, s[0], s[1], s[2]);
    }
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

int main(int argc, char **argv)
{
    g_encode = !(argc > 2 && !strcmp(argv[2], "--plain"));
    g_main_tid = GetCurrentThreadId();
    g_pool_escape = &g_pool;
    g_pool.blocks[0] = g_block0;
    name("None"); name("ByteProperty");
    /* code: lea rcx,[rip+PE_STR] ; call +0 ; call mock_pe   (the 2nd E8 is the ProcessEvent call) */
    DWORD old; VirtualProtect(g_code, sizeof g_code, PAGE_EXECUTE_READWRITE, &old);
    u8 *c = g_code; c[0] = 0x48; c[1] = 0x8D; c[2] = 0x0D; *(int32_t *)(c + 3) = (int32_t)((u8 *)PE_PTR - (c + 7));
    c[7] = 0xE8; *(int32_t *)(c + 8) = 0; c[12] = 0xE8; *(int32_t *)(c + 13) = (int32_t)((u8 *)mock_pe - (c + 17));
    for (int k = 3; k < 7; k++) if (c[k] == 0xE8) { printf("SKIP: E8 in the lea displacement\n"); return 2; }
    /* GUObjectArray reference: mov rax,[rip+g_array] ; mov rcx,[rax+rcx*8] ; mov rax,[rcx+rdx*8] */
    static u8 pat[16]; u8 *p = pat; p[0] = 0x48; p[1] = 0x8B; p[2] = 0x05; *(int32_t *)(p + 3) = (int32_t)((u8 *)&g_array - (p + 7));
    p[7] = 0x48; p[8] = 0x8B; p[9] = 0x0C; p[10] = 0xC8; p[11] = 0x48; p[12] = 0x8B; p[13] = 0x04; p[14] = 0xD1;
    g_vtable[0x44] = (void *)mock_pe;

    g_engine = obj("/Script/Engine", NULL, NULL);
    g_classclass = obj("Class", NULL, g_engine); AT(g_classclass, 0x10, void *) = g_classclass;
    g_structclass = obj("ScriptStruct", g_classclass, g_engine);
    u8 *scene = type("SceneComponent", NULL, 0), *meshc = type("MeshComponent", scene, 0), *skinned = type("SkinnedMeshComponent", meshc, 0);
    u8 *skc = type("SkeletalMeshComponent", skinned, 0), *skm = type("SkeletalMesh", NULL, 0);
    u8 *mi = type("MaterialInstance", NULL, 0), *mid = type("MaterialInstanceDynamic", mi, 0), *morph = type("MorphTarget", NULL, 0);
    u8 *spv = type("ScalarParameterValue", NULL, 1), *mpi = type("MaterialParameterInfo", NULL, 1);
    /* 14.30 did not give these two by name: out of /Script/Engine here, so the DLL takes them from the properties */
    if (g_encode) AT(spv, 0x20, void *) = AT(mpi, 0x20, void *) = obj("/Script/Other", NULL, NULL);
    prop(scene, "AttachParent", 0x100); prop(scene, "RelativeScale3D", 0x108);
    prop(meshc, "OverrideMaterials", 0x130); prop(skinned, "SkeletalMesh", 0x120);
    prop(skm, "MorphTargets", 0x100); u8 *scalars = prop(mi, "ScalarParameterValues", 0x100);
    u8 *inner = calloc(1, 0x80); AT(scalars, 0x78, void *) = inner; AT(inner, 0x78, void *) = spv;   /* Inner, Struct */
    AT(prop(spv, "ParameterInfo", 0), 0x78, void *) = mpi; prop(spv, "ParameterValue", 16); AT(spv, 0x58, int32_t) = 36; AT(spv, 0x5C, int32_t) = 4;
    prop(mpi, "Name", 0);
    g_setmorph = func(skc, "SetMorphTarget", 16); prop(g_setmorph, "MorphTargetName", 0); prop(g_setmorph, "Value", 8); prop(g_setmorph, "bRemoveZeroWeight", 12);
    g_getmorph = func(skc, "GetMorphTarget", 12); prop(g_getmorph, "MorphTargetName", 0); prop(g_getmorph, "ReturnValue", 8);
    g_setscale = func(scene, "SetRelativeScale3D", 12); prop(g_setscale, "NewScale3D", 0);
    for (int i = 0; i < 1200; i++) obj("Filler", g_classclass, g_engine);

    /* the character: base mesh <- body part (mesh with 2 morphs, a MID with Arms 200 % and Height 150 %) */
    u8 *base = obj("CharacterMesh0", skc, g_engine); float *bs = (float *)(base + 0x108); bs[0] = bs[1] = bs[2] = 1;
    u8 *mesh = obj("Body", skm, g_engine);
    static void *morphs[2]; morphs[0] = obj("FNBody_Arms", morph, mesh); morphs[1] = obj("FNBody_Chest", morph, mesh);
    AT(mesh, 0x100, void **) = morphs; AT(mesh, 0x108, int32_t) = 2;
    u8 *m = obj("MaterialInstanceDynamic_0", mid, g_engine);
    static u8 params[3 * 36];
    *(uint32_t *)(params + 0) = name("Tint"); *(float *)(params + 16) = 1;
    *(uint32_t *)(params + 36) = name("FNBody_Arms"); *(float *)(params + 36 + 16) = 200;
    *(uint32_t *)(params + 72) = name("FNBody_Height"); *(float *)(params + 72 + 16) = 150;
    AT(m, 0x100, void *) = params; AT(m, 0x108, int32_t) = 3;
    u8 *body = obj("Body_Part", skc, g_engine);
    AT(body, 0x100, void *) = base; AT(body, 0x120, void *) = mesh;
    static void *mats[1]; mats[0] = m; AT(body, 0x130, void **) = mats; AT(body, 0x138, int32_t) = 1;
    float *bds = (float *)(body + 0x108); bds[0] = bds[1] = bds[2] = 1;

    WNDCLASSA wc = { 0 }; wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "UnrealWindow";
    RegisterClassA(&wc);
    HWND w = CreateWindowA("UnrealWindow", "mock", WS_OVERLAPPED, 0, 0, 10, 10, NULL, NULL, wc.hInstance, NULL);
    (void)w;   /* never shown */

    char dir[MAX_PATH], dll[MAX_PATH], log[MAX_PATH];
    snprintf(dir, sizeof dir, "%s", argc > 1 ? argv[1] : ".");
    snprintf(log, sizeof log, "%s\\fnmods.log", dir); DeleteFileA(log);
    SetEnvironmentVariableA("FNOFFLINE_SANDBOX", dir);
    snprintf(dll, sizeof dll, "%s\\fnmods.dll", dir);
    if (!LoadLibraryA(dll)) { printf("FAIL: LoadLibrary %lu\n", GetLastError()); return 1; }
    /* the same code under a second name (an old launcher injects it as fnpakpatch.dll): must stay idle */
    char dll2[MAX_PATH]; snprintf(dll2, sizeof dll2, "%s\\fnmods-copy.dll", dir);
    if (!CopyFileA(dll, dll2, FALSE) || !LoadLibraryA(dll2)) { printf("FAIL: second copy %lu\n", GetLastError()); return 1; }
    DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < 6000) {
        MSG msg; while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
        Sleep(16);
        if (!g_reset_at && GetTickCount() - t0 > 4000) { g_reset_at = GetTickCount(); for (int k2 = 0; k2 < g_nw; k2++) g_wval[k2] = 0; }
    }
    DWORD repair = g_fixed_at ? g_fixed_at - g_reset_at : 9999;
    FILE *f = fopen(log, "r"); char line[512];
    if (f) { while (fgets(line, sizeof line, f)) fputs(line, stdout); fclose(f); }
    int ok = g_morph_calls > 0 && g_arms > 0.1110f && g_arms < 0.1112f && g_scale_calls == 1 && g_scale > 1.499f && g_scale < 1.501f && !g_bad_thread && repair <= 100
             && bds[0] == 1.0f;
    printf("%s: %d SetMorphTarget calls (Arms weight %.4f, expected 0.1111), %d SetRelativeScale3D (%.3f on the base mesh, expected 1.5 once), %lu calls off the window thread, weights reset by the engine repaired in %lu ms (max 100)\n",
           ok ? "PASS" : "FAIL", g_morph_calls, g_arms, g_scale_calls, g_scale, g_bad_thread, repair);
    fflush(stdout);
    TerminateProcess(GetCurrentProcess(), ok ? 0 : 1);   /* the DLL's thread keeps running otherwise */
    return ok ? 0 : 1;
}
