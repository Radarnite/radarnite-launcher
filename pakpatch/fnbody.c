/*
 * fnbody.dll - the shape side of the body sliders of fn-port outfits in Fortnite 14.30 (injected by the launcher into
 * the game client only: Session.LoadBodySliders, patches\fnbody.dll; idle if it ends up in the -nullrhi match server).
 *
 * fn-port gives every ported outfit one FortCosmeticFloatSliderVariant style per body region (the class of the build's
 * own "Corruption" slider): the game draws it as a slider in its "Edit style" screen, keeps the value in the profile and
 * writes it to the material parameter FNBody_<Region> of the outfit's material instances. fn-port also bakes a morph
 * target FNBody_<Region> into each part mesh. This DLL is the bridge between the two, and changes no byte of the game's
 * code (14.x checks its own code; see fnredirect.c):
 *   - it runs on the game thread, from a WH_GETMESSAGE hook on the thread that owns the game window, woken about once a
 *     frame (16 ms) by a posted thread message (UE pumps messages once per frame with PeekMessage);
 *   - every 250 ms, and as soon as a known component dies or changes mesh (the locker re-applies the style), it lists the live USkeletalMeshComponents (GUObjectArray) whose mesh has FNBody_* morph targets or
 *     whose material instances (OverrideMaterials, UMaterialInstanceDynamic::ScalarParameterValues) carry FNBody_*
 *     parameters; components are grouped by the root of their attachment (one character);
 *   - every tick, for each character, it calls USkeletalMeshComponent::SetMorphTarget(FNBody_<Region>, weight) when the
 *     engine's weight (GetMorphTarget) differs
 *     on its components with those morphs (weight = (s - 1) / (RefScale - 1), s = slider % / 100, RefScale = 10: the
 *     scale fn-port baked the deltas at), and Height scales the part meshes' parent component (SetRelativeScale3D,
 *     from the scale it had).
 * Engine layout (UE 4.26 as Fortnite 14.30 ships it; the same values the match server DLL uses, gameserver-reboot
 * addresses.cpp / finder.h): UObject {vtable, ObjectFlags 0x8, InternalIndex 0xC, ClassPrivate 0x10, NamePrivate 0x18,
 * OuterPrivate 0x20}, UField::Next 0x28, UStruct {SuperStruct 0x40, Children 0x48, ChildProperties 0x50,
 * PropertiesSize 0x58, MinAlignment 0x5C}, FField {Next 0x20, NamePrivate 0x28}, FProperty::Offset_Internal 0x4C,
 * FUObjectItem 24 bytes in chunks of 65536, FNamePool {lock, CurrentBlock, CurrentByteCursor, Blocks[8192]} with
 * FNameEntry header (Len << 6 | hash << 1 | bIsWide), 2-byte stride. Addresses: GUObjectArray by the pattern
 * reboot uses for 4.21+ builds, ProcessEvent by the call after the "UMeshNetworkComponent::ProcessEvent" string
 * (reboot, 14.x+) checked against a live object's vtable, the name pool found in the exe's data by its first entries
 * ("None", "ByteProperty"; 14.30 XOR-encodes the strings, decoded with the key measured on those two). Property
 * offsets come from the engine's own reflection, by name.
 * Every step is logged to <FNOFFLINE_SANDBOX>\fnbody.log. A fault in this DLL's own code disables it (the game goes on).
 * Not verified: everything at run time (written without a game session); the log says which step failed.
 *
 * Build: zig cc -target x86_64-windows-gnu -O2 -shared -s -o fnbody.dll fnbody.c -luser32
 */
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <wchar.h>

/* ---------------------------------------------------------------- log */
static char g_log[MAX_PATH];
static CRITICAL_SECTION g_log_lock;
static void logf_(const char *fmt, ...)
{
    if (!g_log[0]) return;
    EnterCriticalSection(&g_log_lock);
    FILE *f = fopen(g_log, "a");
    if (f) {
        SYSTEMTIME t; GetLocalTime(&t);
        fprintf(f, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
        va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a);
        fputc('\n', f); fclose(f);
    }
    LeaveCriticalSection(&g_log_lock);
}

/* ---------------------------------------------------------------- the sliders (fn-port tools/fn-port/BodySliders.cs) */
#define NREG 12
static const char *REG_NAME[NREG] = { "Height", "Head", "Neck", "Shoulders", "Chest", "Waist", "Glutes", "Arms", "Forearms", "Hands", "Thighs", "Calves" };
static const float REG_MIN[NREG] = { 50, 25, 10, 10, 0, 10, 0, 10, 10, 25, 10, 10 };
static const float REG_MAX[NREG] = { 200, 400, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 400, 1000, 1000 };
#define REF_SCALE 10.0f
#define PREFIX "FNBody_"

/* ---------------------------------------------------------------- engine layout (UE 4.26, Fortnite 14.30) */
#define O_FLAGS 0x08
#define O_INDEX 0x0C
#define O_CLASS 0x10
#define O_NAME 0x18
#define O_OUTER 0x20
#define F_NEXT 0x28            /* UField::Next */
#define S_SUPER 0x40
#define S_CHILDREN 0x48
#define S_CHILDPROPS 0x50
#define S_PROPSIZE 0x58
#define S_MINALIGN 0x5C
#define FF_NEXT 0x20           /* FField::Next */
#define FF_NAME 0x28           /* FField::NamePrivate */
#define P_OFFSET 0x4C          /* FProperty::Offset_Internal */
#define RF_ClassDefaultObject 0x10
#define RF_ArchetypeObject 0x20
#define RF_NeedLoad 0x200
#define EIF_Unreachable (1 << 28)
#define EIF_PendingKill (1 << 29)

typedef unsigned char u8;
#define AT(p, off, T) (*(T *)((u8 *)(p) + (off)))

typedef struct { u8 **objects; u8 *prealloc; int32_t max, num, maxchunks, numchunks; } chunked_array;
static chunked_array *g_objs;
static u8 **g_name_blocks;            /* FNamePool::Blocks */
static int g_len_shift = 6;           /* FNameEntryHeader: Len in the top bits (UE 4.26: << 6), measured on "ByteProperty" */
/* Fortnite 14.30 keeps the ANSI name strings XOR-encoded: byte i ^ k_i, k_{i+1} = k_i + step + i (seen in its name pool:
   k_0 0xE0, step 7). Both are measured on "None" and "ByteProperty"; plain pools (the mock's --plain) leave them off. */
static int g_name_enc; static u8 g_name_k0, g_name_step;
static int name_decode(u8 *out, const u8 *in, int len, u8 k0, u8 step)
{
    u8 k = k0;
    for (int i = 0; i < len; i++) { out[i] = in[i] ^ k; k = (u8)(k + step + i); }
    return len;
}
typedef void (*process_event_fn)(void *obj, void *fn, void *parms);
static process_event_fn g_pe;
static u8 *g_base; static SIZE_T g_size;
static HMODULE g_self; static u8 *g_self_lo, *g_self_hi;

/* ---------------------------------------------------------------- safe reads (discovery only) */
static int rd(const void *p, void *out, SIZE_T n)
{
    SIZE_T got = 0;
    return p && ReadProcessMemory(GetCurrentProcess(), p, out, n, &got) && got == n;
}

/* ---------------------------------------------------------------- fault guard: a fault in this DLL's code returns to the guard */
static void *g_jmp[5];          /* only the game thread runs guarded code */
static volatile int g_guarded;
static volatile LONG g_faults;
static LONG WINAPI guard_handler(EXCEPTION_POINTERS *e)
{
    u8 *ip = (u8 *)e->ExceptionRecord->ExceptionAddress;
    if (!g_guarded || e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || ip < g_self_lo || ip >= g_self_hi) return EXCEPTION_CONTINUE_SEARCH;
    InterlockedIncrement(&g_faults);
    g_guarded = 0;
    __builtin_longjmp(g_jmp, 1);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ---------------------------------------------------------------- module scanning */
static int readable(DWORD prot) { prot &= 0xFF; return prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE || prot == PAGE_EXECUTE_WRITECOPY || prot == PAGE_READONLY || prot == PAGE_READWRITE || prot == PAGE_WRITECOPY; }

/* pattern: bytes, mask '?' = any; calls cb for each match in committed readable pages of the image; stops when cb != 0 */
static u8 *scan(const u8 *pat, const char *mask, int n, int (*cb)(u8 *, void *), void *ctx, int *count)
{
    u8 *first = NULL; if (count) *count = 0;
    for (u8 *p = g_base; p < g_base + g_size; ) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(p, &mi, sizeof mi)) break;
        u8 *end = (u8 *)mi.BaseAddress + mi.RegionSize; if (end > g_base + g_size) end = g_base + g_size;
        if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) && readable(mi.Protect) && end - p > n) {
            for (u8 *q = p; q + n <= end; q++) {
                if (q[0] != pat[0]) continue;
                int k = 1; for (; k < n; k++) if (mask[k] != '?' && q[k] != pat[k]) break;
                if (k < n) continue;
                if (count) (*count)++;
                if (!first) first = q;
                if (cb && cb(q, ctx)) return q;
            }
        }
        p = end;
    }
    return first;
}

static u8 *find_wstr(const wchar_t *w)
{
    int n = (int)wcslen(w) * 2;
    char mask[512]; if (n > (int)sizeof mask) return NULL;
    memset(mask, 'x', n);
    return scan((const u8 *)w, mask, n, NULL, NULL, NULL);
}

static int lea_cb(u8 *q, void *ctx) { return q + 7 + *(int32_t *)(q + 3) == (u8 *)ctx; }
/* first "lea r64, [rip+x]" whose target is s */
static u8 *find_lea(u8 *s)
{
    for (u8 *p = g_base; p < g_base + g_size; ) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(p, &mi, sizeof mi)) break;
        u8 *end = (u8 *)mi.BaseAddress + mi.RegionSize; if (end > g_base + g_size) end = g_base + g_size;
        if (mi.State == MEM_COMMIT && (mi.Protect & 0xF0) && !(mi.Protect & PAGE_GUARD))   /* executable */
            for (u8 *q = p; q + 7 <= end; q++)
                if ((q[0] == 0x48 || q[0] == 0x4C) && q[1] == 0x8D && (q[2] & 0xC7) == 0x05 && lea_cb(q, s)) return q;
        p = end;
    }
    return NULL;
}

/* ---------------------------------------------------------------- names */
static int name_str(uint32_t idx, char *out, int cap)
{
    uint32_t block = idx >> 16, off = idx & 0xFFFF;
    if (!g_name_blocks || block >= 8192 || cap < 2) return 0;
    u8 *b = g_name_blocks[block]; if (!b) return 0;
    u8 *e = b + 2 * off; uint16_t h = *(uint16_t *)e; int len = h >> g_len_shift;
    if (len <= 0 || len >= 1024) return 0;
    if (len >= cap) len = cap - 1;
    if (h & 1) { const uint16_t *w = (const uint16_t *)(e + 2); for (int i = 0; i < len; i++) out[i] = w[i] < 128 ? (char)w[i] : '?'; }
    else if (g_name_enc) name_decode((u8 *)out, e + 2, len, g_name_k0, g_name_step);
    else memcpy(out, e + 2, len);
    out[len] = 0;
    return len;
}
static int name_is(uint32_t idx, const char *s) { char b[256]; return name_str(idx, b, sizeof b) && !strcmp(b, s); }
static int obj_name_is(void *o, const char *s) { return o && name_is(AT(o, O_NAME, uint32_t), s); }

/* the pool: a qword in the exe's data pointing at a block that starts "None", "ByteProperty" (the first names) */
static int pool_ok(u8 **blocks)
{
    u8 *b0; u8 head[24];
    if (!rd(blocks, &b0, 8) || !b0 || !rd(b0, head, sizeof head)) return 0;
    uint16_t h0 = *(uint16_t *)head, h1 = *(uint16_t *)(head + 6);
    /* the length's position in the header, as Dumper-7 measures it (NameArray.cpp): the shift that gives 4 and 12 */
    int sh = 0;
    for (int k = 1; k < 16 && !sh; k++) if ((h0 >> k) == 4 && (h1 >> k) == 12) sh = k;
    if (!sh) return 0;
    int enc = 0; u8 k0 = 0, step = 0;
    if (memcmp(head + 2, "None", 4) || memcmp(head + 8, "ByteProperty", 12)) {
        u8 a[4], b[12];
        k0 = head[2] ^ 'N'; step = (u8)((head[3] ^ 'o') - k0);
        name_decode(a, head + 2, 4, k0, step); name_decode(b, head + 8, 12, k0, step);
        if (memcmp(a, "None", 4) || memcmp(b, "ByteProperty", 12)) return 0;
        enc = 1;
    }
    g_len_shift = sh; g_name_enc = enc; g_name_k0 = k0; g_name_step = step;
    return 1;
}
static u8 **find_name_pool(void)
{
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(g_base + ((IMAGE_DOS_HEADER *)g_base)->e_lfanew);
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (!(s[i].Characteristics & IMAGE_SCN_MEM_WRITE) || (s[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        u8 *p = g_base + s[i].VirtualAddress, *end = p + s[i].Misc.VirtualSize;
        for (u8 *page = p; page < end; ) {
            MEMORY_BASIC_INFORMATION mi;
            if (!VirtualQuery(page, &mi, sizeof mi)) break;
            u8 *rend = (u8 *)mi.BaseAddress + mi.RegionSize; if (rend > end) rend = end;
            if (mi.State == MEM_COMMIT && readable(mi.Protect) && !(mi.Protect & PAGE_GUARD))
                for (uint64_t *q = (uint64_t *)((((uintptr_t)page + 7) & ~7ull) + 8); (u8 *)(q + 4) <= rend; q++) {
                    /* FNamePool: q[-1] = CurrentBlock | CurrentByteCursor << 32, q[0] = Blocks[0], q[CurrentBlock] the last block */
                    uint64_t cur = q[-1]; uint32_t cb = (uint32_t)cur, cursor = (uint32_t)(cur >> 32);
                    if (cb >= 8192 || cursor > 0x20000 || q[0] < 0x10000 || q[0] >= 0x800000000000ull) continue;
                    if ((u8 *)(q + cb + 2) > rend || !q[cb] || q[cb + 1]) continue;
                    if (pool_ok((u8 **)q)) return (u8 **)q;
                }
            page = rend;
        }
    }
    return NULL;
}

/* once, when the pool is not found: every qword of the exe's data pointing at a block that starts like the name pool
   (header lengths 4 and 12, or "None"), FNamePool-shaped (as find_name_pool checks it) or not, with its first 32 bytes */
static void pool_diag(void)
{
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(g_base + ((IMAGE_DOS_HEADER *)g_base)->e_lfanew);
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    int shaped = 0, shown = 0, loose = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (!(s[i].Characteristics & IMAGE_SCN_MEM_WRITE) || (s[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        u8 *p = g_base + s[i].VirtualAddress, *end = p + s[i].Misc.VirtualSize;
        for (u8 *page = p; page < end; ) {
            MEMORY_BASIC_INFORMATION mi;
            if (!VirtualQuery(page, &mi, sizeof mi)) break;
            u8 *rend = (u8 *)mi.BaseAddress + mi.RegionSize; if (rend > end) rend = end;
            if (mi.State == MEM_COMMIT && readable(mi.Protect) && !(mi.Protect & PAGE_GUARD))
                for (uint64_t *q = (uint64_t *)((((uintptr_t)page + 7) & ~7ull) + 8); (u8 *)(q + 4) <= rend; q++) {
                    uint64_t cur = q[-1]; uint32_t cb = (uint32_t)cur, cursor = (uint32_t)(cur >> 32);
                    if (q[0] < 0x10000 || q[0] >= 0x800000000000ull || (q[0] & 1)) continue;
                    int shape = cb < 8192 && cursor <= 0x20000 && (u8 *)(q + cb + 2) <= rend && q[cb] && !q[cb + 1];
                    u8 h[32]; if (!rd((void *)q[0], h, sizeof h)) continue;
                    /* UE 4.26's header: Len << 6; "None" then "ByteProperty" give 4 and 12 whatever their bytes */
                    uint16_t h0 = *(uint16_t *)h, h1 = *(uint16_t *)(h + 6);
                    int sh = (h0 >> 6) == 4 && (h1 >> 6) == 12 ? 6 : 0;
                    int none = !memcmp(h + 2, "None", 4);
                    if (!sh && !none) continue;
                    if (shape) shaped++; else loose++;
                    if (shape ? shown++ >= 40 : loose > 15) continue;
                    char hex[3 * sizeof h + 1]; for (int k = 0; k < (int)sizeof h; k++) sprintf(hex + 3 * k, "%02x ", h[k]);
                    logf_("%s candidate exe+0x%llx: CurrentBlock %u cursor 0x%x, lengths 4/12 at shift 6 %s, \"None\" at 2 %s; block 0: %s",
                          shape ? "pool" : "loose", (unsigned long long)((u8 *)q - g_base), cb, cursor, sh ? "yes" : "no", none ? "yes" : "no", hex);
                }
            page = rend;
        }
    }
    logf_("pool diagnosis: %d pool-shaped candidates, %d other pointers to a None/ByteProperty-like block", shaped, loose);
}

/* ---------------------------------------------------------------- objects */
static int g_chunk = 65536;
static u8 *obj_item(int32_t i)
{
    if (i < 0 || i >= g_objs->num) return NULL;
    u8 *chunk = g_objs->objects[i / g_chunk];
    return chunk ? chunk + (i % g_chunk) * 24 : NULL;
}
static void *obj_at(int32_t i, int32_t *iflags)
{
    u8 *it = obj_item(i); if (!it) return NULL;
    if (iflags) *iflags = AT(it, 8, int32_t);
    return AT(it, 0, void *);
}
/* still the live object it was (slot not reused) */
static int alive(void *o, int32_t serial)
{
    if (!o) return 0;
    u8 *it = obj_item(AT(o, O_INDEX, int32_t));
    return it && AT(it, 0, void *) == o && AT(it, 16, int32_t) == serial && !(AT(it, 8, int32_t) & (EIF_PendingKill | EIF_Unreachable));
}
static int32_t serial_of(void *o) { u8 *it = obj_item(AT(o, O_INDEX, int32_t)); return it ? AT(it, 16, int32_t) : 0; }
static int is_a(void *o, void *cls)
{
    for (void *c = AT(o, O_CLASS, void *); c; c = AT(c, S_SUPER, void *)) if (c == cls) return 1;
    return 0;
}
/* classes and script structs of /Script/Engine by name, in one pass over the objects */
typedef struct { const char *kind, *name; void *found; } type_req;
static void find_types(type_req *req, int n)
{
    for (int32_t i = 0; i < g_objs->num; i++) {
        void *o = obj_at(i, NULL); char b[128];
        if (!o || !name_str(AT(o, O_NAME, uint32_t), b, sizeof b)) continue;
        for (int k = 0; k < n; k++) {
            if (req[k].found || strcmp(b, req[k].name)) continue;
            void *c = AT(o, O_CLASS, void *), *outer = AT(o, O_OUTER, void *);
            if (obj_name_is(c, req[k].kind) && obj_name_is(outer, "/Script/Engine")) req[k].found = o;
        }
    }
}
static void *find_prop(void *st, const char *name)
{
    for (void *s = st; s; s = AT(s, S_SUPER, void *))
        for (void *f = AT(s, S_CHILDPROPS, void *); f; f = AT(f, FF_NEXT, void *))
            if (name_is(AT(f, FF_NAME, uint32_t), name)) return f;
    return NULL;
}
static int prop_off(void *st, const char *name) { void *f = find_prop(st, name); return f ? AT(f, P_OFFSET, int32_t) : -1; }
static void *find_func(void *cls, const char *name)
{
    for (void *s = cls; s; s = AT(s, S_SUPER, void *))
        for (void *f = AT(s, S_CHILDREN, void *); f; f = AT(f, F_NEXT, void *))
            if (obj_name_is(f, name)) return f;
    return NULL;
}

/* the script struct a struct or array-of-struct property points at (FStructProperty::Struct, FArrayProperty::Inner:
   0x78 in UE 4.26, right after FProperty; the neighbours too), checked to be a live object of class ScriptStruct */
static void *struct_of(void *f)
{
    static const int offs[] = { 0x78, 0x80, 0x70 };
    for (int k = 0; k < 3; k++) {
        void *st = NULL; int32_t idx;
        if (!rd((u8 *)f + offs[k], &st, 8) || !st || !rd((u8 *)st + O_INDEX, &idx, 4) || obj_at(idx, NULL) != st) continue;
        if (obj_name_is(AT(st, O_CLASS, void *), "ScriptStruct")) return st;
    }
    return NULL;
}
static void *array_struct_of(void *f)
{
    static const int offs[] = { 0x78, 0x80, 0x70 };
    for (int k = 0; k < 3; k++) {
        void *inner = NULL; uint32_t n;
        if (!rd((u8 *)f + offs[k], &inner, 8) || !inner || !rd((u8 *)inner + FF_NAME, &n, 4)) continue;
        void *st = struct_of(inner); if (st) return st;
    }
    return NULL;
}
static const char *obj_name(void *o, char *b, int cap) { if (!o || !name_str(AT(o, O_NAME, uint32_t), b, cap)) snprintf(b, cap, "?"); return b; }
/* what a property holds from 0x70 on, when struct_of finds nothing there: objects by name and class, FFields by name */
static void dump_prop(const char *what, void *f)
{
    if (!f) { logf_("%s: no such property", what); return; }
    for (int off = 0x70; off <= 0x88; off += 8) {
        void *p = NULL; int32_t idx; uint32_t n; char a[128] = "-", c[128] = "-";
        if (!rd((u8 *)f + off, &p, 8) || !p) { logf_("%s +0x%x: %p", what, off, p); continue; }
        if (rd((u8 *)p + O_INDEX, &idx, 4) && obj_at(idx, NULL) == p) {
            obj_name(p, a, sizeof a); obj_name(AT(p, O_CLASS, void *), c, sizeof c);
            logf_("%s +0x%x: %p object %s of class %s", what, off, p, a, c);
        } else if (rd((u8 *)p + FF_NAME, &n, 4) && name_str(n, a, sizeof a)) logf_("%s +0x%x: %p field %s", what, off, p, a);
        else logf_("%s +0x%x: %p", what, off, p);
    }
}

/* ---------------------------------------------------------------- reflection we need */
static struct {
    void *skc, *mid;                                      /* SkeletalMeshComponent, MaterialInstanceDynamic */
    int o_mesh, o_override, o_attach, o_scale, o_morphs, o_scalars;
    int sp_size, sp_name, sp_value;                       /* FScalarParameterValue: stride, ParameterInfo.Name, ParameterValue */
    void *f_setmorph; int fm_name, fm_value, fm_remove, fm_size;
    void *f_setscale; int fs_scale, fs_size;
} R;

static void objs_diag(void);
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
/* UKismetSystemLibrary::ExecuteConsoleCommand(WorldContextObject, Command, SpecificPlayer) on its default object */
static void console(void *world_ctx, const wchar_t *cmd)
{
    static void *cdo, *fn; static int o_ctx, o_cmd, o_pl, done;
    if (!done) {
        done = 1;
        for (int32_t i = 0; i < g_objs->num && !cdo; i++) { void *o = obj_at(i, NULL); if (o && obj_name_is(o, "Default__KismetSystemLibrary")) cdo = o; }
        if (cdo) fn = find_func(AT(cdo, O_CLASS, void *), "ExecuteConsoleCommand");
        if (fn) { o_ctx = prop_off(fn, "WorldContextObject"); o_cmd = prop_off(fn, "Command"); o_pl = prop_off(fn, "SpecificPlayer"); }
        logf_("test: console %p %p (%d %d %d)", cdo, fn, o_ctx, o_cmd, o_pl);
    }
    if (!cdo || !fn || o_ctx < 0 || o_cmd < 0) return;
    u8 parms[128]; memset(parms, 0, sizeof parms);
    AT(parms, o_ctx, void *) = world_ctx;
    AT(parms, o_cmd, const wchar_t *) = cmd; AT(parms, o_cmd + 8, int32_t) = (int32_t)wcslen(cmd) + 1; AT(parms, o_cmd + 12, int32_t) = (int32_t)wcslen(cmd) + 1;
    g_pe(cdo, fn, parms);
    logf_("test: console \"%ls\" sent", cmd);
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

/* ---------------------------------------------------------------- ragdoll deaths and test hooks */
static void test_on_death(void *pawn, int ragdoll);
static DWORD g_game_tid;   /* the game thread (set once its window is found) */
#include "fnragdoll.c"
#include "fntest.c"

/* ---------------------------------------------------------------- game thread */
static UINT g_msg; static volatile LONG g_pending; static int g_ready, g_dead;
static DWORD g_last_collect;

static int g_body_off;
static void body_tick(void)
{
    if (!g_ready) {
        /* the lobby is still loading the first seconds: try again every 10 s for 2 minutes */
        static int tries; static DWORD next;
        if (GetTickCount() < next) return;
        if (!resolve()) {
            next = GetTickCount() + 10000;
            if (++tries >= 12) { g_body_off = 1; logf_("FAIL: engine types not found: body sliders disabled"); }
            else logf_("engine types not found yet (try %d of 12)", tries);
            return;
        }
        g_ready = 1; g_ready_at = GetTickCount(); logf_("ready");
    }
    DWORD now = GetTickCount();
    /* a full scan every 250 ms, and right away when a known component died or changed mesh (a re-applied style); in
       slices, see collect_step */
    if (g_cur < 0 && (g_rescan || now - g_last_collect >= 250)) { g_rescan = 0; collect_begin(); g_last_collect = now; }
    if (g_cur >= 0) collect_step();
    apply();
    test_step();
}

static void tick(void)
{
    if (g_dead) return;
    g_guarded = 1;
    if (__builtin_setjmp(g_jmp)) {
        g_dead = 1;
        logf_("FAULT in fnbody (reading engine memory): body sliders and ragdolls disabled for this session");
        return;
    }
    ragdoll_tick();
    test_tick();
    if (!g_body_off) body_tick();
    g_guarded = 0;
}

static LRESULT CALLBACK getmsg(int code, WPARAM wp, LPARAM lp)
{
    MSG *m = (MSG *)lp;
    if (code == HC_ACTION && wp == PM_REMOVE && m && m->message == g_msg && !m->hwnd) {
        tick();
        m->message = WM_NULL;
        InterlockedExchange(&g_pending, 0);
    }
    return CallNextHookEx(NULL, code, wp, lp);
}

typedef struct { DWORD pid; HWND w; } findw;
static BOOL CALLBACK enum_cb(HWND w, LPARAM lp)
{
    findw *f = (findw *)lp; DWORD pid = 0; char cls[64];
    GetWindowThreadProcessId(w, &pid);
    if (pid == f->pid && GetClassNameA(w, cls, sizeof cls) && !strcmp(cls, "UnrealWindow")) { f->w = w; return FALSE; }
    return TRUE;
}

/* ---------------------------------------------------------------- setup */
static int objs_cb(u8 *q, void *ctx)
{
    (void)ctx;
    chunked_array *a = (chunked_array *)(q + 7 + *(int32_t *)(q + 3)); chunked_array v;
    if (!rd(a, &v, sizeof v) || !v.objects || v.num < 1000 || v.num > v.max || v.max > (1 << 24) || v.numchunks < 1 || v.numchunks > v.maxchunks) return 0;
    u8 *c0; void *o0; if (!rd(v.objects, &c0, 8) || !rd(c0, &o0, 8) || !o0) return 0;
    /* the code pattern matches more than one chunked array in 14.30: keep the biggest (GUObjectArray) */
    if (!g_objs || v.num > g_objs->num) g_objs = a;
    return 0;
}
static const u8 PAT_OBJS[] = { 0x48,0x8B,0x05,0,0,0,0, 0x48,0x8B,0x0C,0xC8, 0x48,0x8B,0x04,0xD1 };
/* diagnosis: every chunked array the GUObjectArray pattern points at, and the words around the one taken */
static int objs_diag_cb(u8 *q, void *ctx)
{
    int *n = (int *)ctx; chunked_array *a = (chunked_array *)(q + 7 + *(int32_t *)(q + 3)); chunked_array v;
    if (++*n > 12 || !rd(a, &v, sizeof v)) return 0;
    logf_("object array candidate exe+0x%llx (code exe+0x%llx): objects %p, max %d, num %d, chunks %d/%d", (unsigned long long)((u8 *)a - g_base),
          (unsigned long long)(q - g_base), v.objects, v.max, v.num, v.numchunks, v.maxchunks);
    return 0;
}
static void objs_diag(void)
{
    int n = 0; scan(PAT_OBJS, "xxx????xxxxxxxx", sizeof PAT_OBJS, objs_diag_cb, &n, NULL);
    int32_t w[16]; if (rd((u8 *)g_objs - 0x20, w, sizeof w))
        logf_("around exe+0x%llx: -0x20: %d %d %d %d %d %d %d %d | %d %d %d %d %d %d %d %d", (unsigned long long)((u8 *)g_objs - g_base),
              w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11], w[12], w[13], w[14], w[15]);
}

static u8 *find_process_event(void)
{
    u8 *s = find_wstr(L"UMeshNetworkComponent::ProcessEvent: Invalid mesh network node type: %s");
    if (!s) { logf_("ProcessEvent: string not found"); return NULL; }
    u8 *ref = find_lea(s);
    if (!ref) { logf_("ProcessEvent: no reference to its string"); return NULL; }
    /* the 2nd E8 byte after the reference is the call to UObject::ProcessEvent (gameserver-reboot finder.h, 14.x) */
    int seen = 0;
    for (int i = 0; i < 2000; i++) if (ref[i] == 0xE8 && ++seen == 2) return ref + i + 5 + *(int32_t *)(ref + i + 1);
    logf_("ProcessEvent: no call after the reference");
    return NULL;
}
/* the address is ProcessEvent if a live object's vtable holds it */
static int in_vtable(u8 *fn)
{
    for (int32_t i = 0; i < 64; i++) {
        void *o; if (!rd(&g_objs->objects[0], &o, 8)) return 0;
        u8 *it = (u8 *)o + i * 24; void *obj, **vt; if (!rd(it, &obj, 8) || !obj || !rd(obj, &vt, 8)) continue;
        for (int k = 0; k < 0x100; k++) { void *f; if (!rd(vt + k, &f, 8)) break; if (f == fn) { logf_("ProcessEvent: vtable index 0x%x of object %d", k, i); return 1; } }
    }
    return 0;
}

static DWORD WINAPI worker(LPVOID unused)
{
    (void)unused;
    test_parse();
    ragdoll_init();
    test_init();
    int i;
    for (i = 0; i < 600 && !g_objs; i++) { scan(PAT_OBJS, "xxx????xxxxxxxx", sizeof PAT_OBJS, objs_cb, NULL, NULL); if (!g_objs) Sleep(500); }
    if (!g_objs) { logf_("FAIL: GUObjectArray not found after %d s", i / 2); return 0; }
    logf_("GUObjectArray at exe+0x%llx (%d objects) after %d s", (unsigned long long)((u8 *)g_objs - g_base), g_objs->num, i / 2);
    u8 *pe = NULL;
    for (i = 0; i < 120 && !pe; i++) { pe = find_process_event(); if (!pe) Sleep(1000); }
    if (!pe || !in_vtable(pe)) { logf_("FAIL: ProcessEvent not found or not in a vtable (%p)", pe); return 0; }
    g_pe = (process_event_fn)pe;
    logf_("ProcessEvent at exe+0x%llx", (unsigned long long)(pe - g_base));
    for (i = 0; i < 120 && !g_name_blocks; i++) { g_name_blocks = find_name_pool(); if (!g_name_blocks) { if (!i) pool_diag(); Sleep(1000); } }
    if (!g_name_blocks) { logf_("FAIL: name pool not found"); return 0; }
    logf_("name pool blocks at exe+0x%llx (entry length shift %d, %s)", (unsigned long long)((u8 *)g_name_blocks - g_base), g_len_shift,
          g_name_enc ? "strings XOR-encoded" : "plain strings");
    if (g_name_enc) logf_("name strings: k0 0x%02x, step %u", g_name_k0, g_name_step);
    findw f = { GetCurrentProcessId(), NULL };
    for (i = 0; i < 1200 && !f.w; i++) { EnumWindows(enum_cb, (LPARAM)&f); if (!f.w) Sleep(500); }
    if (!f.w) { logf_("FAIL: no game window"); return 0; }
    g_game_tid = GetWindowThreadProcessId(f.w, NULL);
    g_msg = RegisterWindowMessageA("RadarniteFnBodyTick");
    HHOOK h = SetWindowsHookExW(WH_GETMESSAGE, getmsg, g_self, g_game_tid);
    if (!h) { logf_("FAIL: SetWindowsHookEx %lu", GetLastError()); return 0; }
    logf_("game thread %lu hooked (message 0x%x); waking it every 16 ms", g_game_tid, g_msg);
    DWORD posted = 0;
    while (!g_dead) {
        Sleep(16);   /* about once a frame: a slider drag shows on the body in the same frame the game writes it */
        if (InterlockedCompareExchange(&g_pending, 1, 0) == 0) {
            if (!PostThreadMessageA(g_game_tid, g_msg, 0, 0)) { InterlockedExchange(&g_pending, 0); if (!IsWindow(f.w)) break; }
            else posted++;
        }
    }
    UnhookWindowsHookEx(h);
    logf_("stopped after %lu ticks (faults %ld)", posted, g_faults);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    (void)r;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        g_self = h;
        InitializeCriticalSection(&g_log_lock);
        char sb[MAX_PATH];
        DWORD n = GetEnvironmentVariableA("FNOFFLINE_SANDBOX", sb, sizeof sb);
        if (n && n < sizeof sb - 16) snprintf(g_log, sizeof g_log, "%s\\fnbody.log", sb);
        const char *cmd = GetCommandLineA();
        if (cmd && strstr(cmd, "-nullrhi")) { logf_("fnbody: match server process (-nullrhi): nothing to draw, not started"); return TRUE; }
        /* one copy per process (the same code loaded under two names would scale Height twice) */
        char mx[64]; snprintf(mx, sizeof mx, "Local\\RadarniteFnBody-%lu", GetCurrentProcessId());
        if (CreateMutexA(NULL, FALSE, mx) && GetLastError() == ERROR_ALREADY_EXISTS) { logf_("fnbody: already running in this process (another copy): this one stays idle"); return TRUE; }
        g_base = (u8 *)GetModuleHandleW(NULL);
        IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(g_base + ((IMAGE_DOS_HEADER *)g_base)->e_lfanew);
        g_size = nt->OptionalHeader.SizeOfImage;
        IMAGE_NT_HEADERS64 *me = (IMAGE_NT_HEADERS64 *)((u8 *)h + ((IMAGE_DOS_HEADER *)h)->e_lfanew);
        g_self_lo = (u8 *)h; g_self_hi = (u8 *)h + me->OptionalHeader.SizeOfImage;
        AddVectoredExceptionHandler(1, guard_handler);
        logf_("fnbody loaded into pid %lu", GetCurrentProcessId());
        HANDLE t = CreateThread(NULL, 0, worker, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
