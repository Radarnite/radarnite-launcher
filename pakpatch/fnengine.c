/*
 * fnengine.c - the engine layer of fnmods.dll (#included by fnmods.c): everything the modules share to read and drive
 * Fortnite 14.30's engine without changing a byte of its code (14.x checks its own code; see fnredirect.c).
 *
 * Engine layout (UE 4.26 as Fortnite 14.30 ships it; the same values the match server DLL uses, gameserver-reboot
 * addresses.cpp / finder.h): UObject {vtable, ObjectFlags 0x8, InternalIndex 0xC, ClassPrivate 0x10, NamePrivate 0x18,
 * OuterPrivate 0x20}, UField::Next 0x28, UStruct {SuperStruct 0x40, Children 0x48, ChildProperties 0x50,
 * PropertiesSize 0x58, MinAlignment 0x5C}, FField {Next 0x20, NamePrivate 0x28}, FProperty::Offset_Internal 0x4C,
 * FUObjectItem 24 bytes in chunks of 65536, FNamePool {lock, CurrentBlock, CurrentByteCursor, Blocks[8192]} with
 * FNameEntry header (Len << 6 | hash << 1 | bIsWide), 2-byte stride. Addresses: GUObjectArray by the pattern
 * reboot uses for 4.21+ builds, ProcessEvent by the call after the "UMeshNetworkComponent::ProcessEvent" string
 * (reboot, 14.x+) checked against a live object's vtable, the name pool found in the exe's data by its first entries
 * ("None", "ByteProperty"; 14.30 XOR-encodes the strings, decoded with the key measured on those two). Everything
 * else comes from the engine's own reflection, by name: classes, properties and their offsets, UFunctions called
 * through ProcessEvent.
 * The fault guard: a fault in this DLL's own code (never the engine's) inside guarded() returns to it; the host turns
 * the faulting module off and the game goes on.
 */

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
static void *g_jmp[5];          /* the innermost guarded() of the guarded thread */
static volatile int g_guarded;
static volatile DWORD g_guard_tid;
static volatile LONG g_faults;
static LONG WINAPI guard_handler(EXCEPTION_POINTERS *e)
{
    u8 *ip = (u8 *)e->ExceptionRecord->ExceptionAddress;
    if (!g_guarded || GetCurrentThreadId() != g_guard_tid || e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || ip < g_self_lo || ip >= g_self_hi)
        return EXCEPTION_CONTINUE_SEARCH;
    InterlockedIncrement(&g_faults);
    g_guarded = 0;
    __builtin_longjmp(g_jmp, 1);
    return EXCEPTION_CONTINUE_SEARCH;
}
/* fn() under the guard (nestable, game thread): 0 = it ran, 1 = it faulted in this DLL's code and was cut short */
static int guarded(void (*fn)(void))
{
    void *saved[5]; memcpy(saved, g_jmp, sizeof saved);
    int was = g_guarded;
    if (__builtin_setjmp(g_jmp)) { memcpy(g_jmp, saved, sizeof saved); g_guarded = was; return 1; }
    g_guard_tid = GetCurrentThreadId();
    g_guarded = 1;
    fn();
    memcpy(g_jmp, saved, sizeof saved); g_guarded = was;
    return 0;
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

/* ---------------------------------------------------------------- shared helpers of the modules */

/* a bit of a bitfield bool property (FBoolProperty: FieldSize 0x78, ByteOffset 0x79, ByteMask 0x7A, FieldMask 0x7B) */
typedef struct { int off; u8 mask; } boolp;
static boolp bool_prop(void *st, const char *name)
{
    boolp b = { -1, 0 };
    void *f = st ? find_prop(st, name) : NULL;
    if (!f) return b;
    b.off = AT(f, P_OFFSET, int32_t) + AT(f, 0x79, u8);
    b.mask = AT(f, 0x7B, u8);
    if (!b.mask) b.mask = 0xFF;   /* a plain bool */
    return b;
}
static int bool_get(void *o, boolp b) { return b.off >= 0 && (AT(o, b.off, u8) & b.mask) != 0; }
static void bool_set(void *o, boolp b, int v) { if (b.off < 0) return; if (v) AT(o, b.off, u8) |= b.mask; else AT(o, b.off, u8) &= (u8)~b.mask; }

/* classes by package and name, in one pass over the objects */
typedef struct { const char *pkg, *name; void *found; } cls_req;
static void find_classes(cls_req *req, int n)
{
    for (int32_t i = 0; i < g_objs->num; i++) {
        void *o = obj_at(i, NULL); char b[128];
        if (!o || !name_str(AT(o, O_NAME, uint32_t), b, sizeof b)) continue;
        for (int k = 0; k < n; k++) {
            if (req[k].found || strcmp(b, req[k].name)) continue;
            void *c = AT(o, O_CLASS, void *), *outer = AT(o, O_OUTER, void *);
            if ((obj_name_is(c, "Class") || obj_name_is(c, "BlueprintGeneratedClass")) && obj_name_is(outer, req[k].pkg)) req[k].found = o;
        }
    }
}
static void *find_class(const char *pkg, const char *name) { cls_req q = { pkg, name, NULL }; find_classes(&q, 1); return q.found; }
static void *find_object_named(const char *name)
{
    for (int32_t i = 0; i < g_objs->num; i++) { void *o = obj_at(i, NULL); if (o && obj_name_is(o, name)) return o; }
    return NULL;
}
/* the first live instance of a class (not a default or archetype object) */
static void *find_instance(void *cls)
{
    if (!cls) return NULL;
    for (int32_t i = 0; i < g_objs->num; i++) {
        int32_t fl; void *o = obj_at(i, &fl);
        if (!o || (fl & (EIF_PendingKill | EIF_Unreachable)) || (AT(o, O_FLAGS, uint32_t) & (RF_ClassDefaultObject | RF_ArchetypeObject))) continue;
        if (is_a(o, cls)) return o;
    }
    return NULL;
}

/* a UFunction call: parameters by name into a zeroed buffer of the function's size */
typedef struct { void *fn; int size; } ufn;
static ufn fn_of(void *cls, const char *name)
{
    ufn f = { NULL, 0 };
    if (!cls) return f;
    f.fn = find_func(cls, name);
    if (f.fn) f.size = AT(f.fn, S_PROPSIZE, int32_t);
    if (f.size < 0 || f.size > 512) f.fn = NULL;
    return f;
}
static int parm(ufn f, const char *name) { return f.fn ? prop_off(f.fn, name) : -1; }
static void call(void *obj, ufn f, u8 *parms) { if (obj && f.fn) g_pe(obj, f.fn, parms); }
static void call0(void *obj, ufn f) { u8 p[512]; memset(p, 0, sizeof p); call(obj, f, p); }
static void call_bool(void *obj, ufn f, const char *pn, int v)
{
    u8 p[512]; memset(p, 0, sizeof p);
    int o = parm(f, pn); if (o < 0 && f.size > 0) o = 0;
    if (o >= 0) p[o] = (u8)v;
    call(obj, f, p);
}
static void call_bool2(void *obj, ufn f, const char *a, int va, const char *b, int vb)
{
    u8 p[512]; memset(p, 0, sizeof p);
    int oa = parm(f, a), ob = parm(f, b);
    if (oa >= 0) p[oa] = (u8)va;
    if (ob >= 0) p[ob] = (u8)vb;
    call(obj, f, p);
}
/* an FString parameter (TArray<TCHAR>) pointing at a caller-owned string: the engine copies what it keeps */
static void put_fstring(u8 *parms, int off, const wchar_t *s)
{
    int32_t n = (int32_t)wcslen(s) + 1;
    AT(parms, off, const wchar_t *) = s; AT(parms, off + 8, int32_t) = n; AT(parms, off + 12, int32_t) = n;
}

/* UKismetSystemLibrary and UGameplayStatics: their default objects and functions, found once */
static void *g_ksl, *g_gps;
static void *ksl(void) { if (!g_ksl) g_ksl = find_object_named("Default__KismetSystemLibrary"); return g_ksl; }
static void *gps(void) { if (!g_gps) g_gps = find_object_named("Default__GameplayStatics"); return g_gps; }

/* UKismetSystemLibrary::ExecuteConsoleCommand(WorldContextObject, Command, SpecificPlayer) on its default object: what
   the console runs when you type the command (exec functions, console variables, engine commands) */
static void console(void *world_ctx, const wchar_t *cmd)
{
    static ufn fn; static int o_ctx = -1, o_cmd = -1, done;
    if (!done) {
        done = 1;
        if (ksl()) fn = fn_of(AT(ksl(), O_CLASS, void *), "ExecuteConsoleCommand");
        o_ctx = parm(fn, "WorldContextObject"); o_cmd = parm(fn, "Command");
        logf_("engine: ExecuteConsoleCommand %p on %p (WorldContextObject %d, Command %d)", fn.fn, ksl(), o_ctx, o_cmd);
    }
    if (!g_ksl || !fn.fn || o_ctx < 0 || o_cmd < 0) return;
    u8 parms[512]; memset(parms, 0, sizeof parms);
    AT(parms, o_ctx, void *) = world_ctx;
    put_fstring(parms, o_cmd, cmd);
    g_pe(g_ksl, fn.fn, parms);
    logf_("exec \"%ls\"", cmd);
}
/* a console variable's value (UKismetSystemLibrary::GetConsoleVariableFloatValue): NAN when the function is missing */
static float cvar_get(const wchar_t *name)
{
    static ufn fn; static int o_name = -1, o_ret = -1, done;
    if (!done) {
        done = 1;
        if (ksl()) fn = fn_of(AT(ksl(), O_CLASS, void *), "GetConsoleVariableFloatValue");
        o_name = parm(fn, "VariableName"); o_ret = parm(fn, "ReturnValue");
        logf_("engine: GetConsoleVariableFloatValue %p (VariableName %d, ReturnValue %d)", fn.fn, o_name, o_ret);
    }
    if (!g_ksl || !fn.fn || o_name < 0 || o_ret < 0) return NAN;
    u8 parms[512]; memset(parms, 0, sizeof parms);
    put_fstring(parms, o_name, name);
    g_pe(g_ksl, fn.fn, parms);
    return AT(parms, o_ret, float);
}
/* an FName from a string (UKismetStringLibrary::Conv_StringToName; the engine adds the name when it is new) */
static uint32_t fname_of(const wchar_t *s)
{
    static void *cdo; static ufn fn; static int o_in = -1, o_ret = -1, done;
    if (!done) {
        done = 1;
        cdo = find_object_named("Default__KismetStringLibrary");
        if (cdo) fn = fn_of(AT(cdo, O_CLASS, void *), "Conv_StringToName");
        o_in = parm(fn, "inString"); if (o_in < 0) o_in = parm(fn, "InString");
        o_ret = parm(fn, "ReturnValue");
    }
    if (!cdo || !fn.fn || o_in < 0 || o_ret < 0) return 0;
    u8 p[512]; memset(p, 0, sizeof p);
    put_fstring(p, o_in, s);
    g_pe(cdo, fn.fn, p);
    return AT(p, o_ret, uint32_t);
}

/* the local player controller (the client has only its own) */
static void *g_pc_cls;
static void *player_controller(void)
{
    if (!g_pc_cls) g_pc_cls = find_class("/Script/Engine", "PlayerController");
    return find_instance(g_pc_cls);
}
/* the same, kept while it lives: a full search at most once a second (it walks every object) */
static void *g_lpc; static int32_t g_lpc_serial; static DWORD g_lpc_next;
static void *local_pc(void)
{
    if (g_lpc && alive(g_lpc, g_lpc_serial)) return g_lpc;
    g_lpc = NULL;
    DWORD now = GetTickCount();
    if (now < g_lpc_next) return NULL;
    g_lpc_next = now + 1000;
    g_lpc = player_controller();
    if (g_lpc) g_lpc_serial = serial_of(g_lpc);
    return g_lpc;
}

/* AActor's ProcessEvent: the entry of Actor's class default object (its vtable is AActor's own). An actor class's
   entry must be that one or UObject's, so a swapped slot is surely ProcessEvent (3 arguments) */
#define VT_SLOTS 0x400
#define PE_INDEX 0x43
static void *actor_pe(void)
{
    static void *pe; static int tried;
    if (tried) return pe;
    tried = 1;
    void *actor = find_class("/Script/Engine", "Actor");
    for (int32_t i = 0; i < g_objs->num && actor && !pe; i++) {
        void *o = obj_at(i, NULL);
        if (!o || AT(o, O_CLASS, void *) != actor || !(AT(o, O_FLAGS, uint32_t) & RF_ClassDefaultObject)) continue;
        void **vt = AT(o, 0, void **); void *e;
        if (vt && rd(vt + PE_INDEX, &e, sizeof e)) pe = e;
    }
    logf_("engine: AActor's ProcessEvent %p (exe+0x%llx)", pe, pe ? (unsigned long long)((u8 *)pe - g_base) : 0ull);
    return pe;
}

/* ---------------------------------------------------------------- what the modules need, found or missing (one table) */
/* kind 'c' class or struct (any package), 'p' property "Class.Member", 'f' function "Class.Member"; opt = the module
   copes without it (another way, or a smaller feature) */
typedef struct { char kind; const char *name; int opt; } need;
/* classes and script structs by name, any package, in one pass over the objects: names[k] -> out[k] */
static void find_types_any(const char **names, void **out, int n)
{
    for (int k = 0; k < n; k++) out[k] = NULL;
    for (int32_t i = 0; i < g_objs->num; i++) {
        void *o = obj_at(i, NULL); char b[128];
        if (!o || !name_str(AT(o, O_NAME, uint32_t), b, sizeof b)) continue;
        for (int k = 0; k < n; k++) {
            if (out[k] || strcmp(b, names[k])) continue;
            void *oc = AT(o, O_CLASS, void *);
            if (obj_name_is(oc, "Class") || obj_name_is(oc, "BlueprintGeneratedClass") || obj_name_is(oc, "ScriptStruct")) out[k] = o;
        }
    }
}
/* one line of the table; cls = the class part of nd->name, found or NULL. 1 = found */
static int need_line(const char *module, const need *nd, void *cls)
{
    const char *dot = strchr(nd->name, '.');
    void *what = cls; int off = -1;
    if (cls && nd->kind == 'p' && dot) { void *f = find_prop(cls, dot + 1); what = f; off = f ? AT(f, P_OFFSET, int32_t) : -1; }
    if (cls && nd->kind == 'f' && dot) what = find_func(cls, dot + 1);
    const char *kind = nd->kind == 'c' ? "class" : nd->kind == 'p' ? "property" : "function";
    if (what && off >= 0) logf_("  %-8s %-8s %-52s found (offset 0x%x)", module, kind, nd->name, off);
    else if (what) logf_("  %-8s %-8s %-52s found", module, kind, nd->name);
    else logf_("  %-8s %-8s %-52s MISSING%s", module, kind, nd->name, nd->opt ? " (optional)" : "");
    return what != NULL;
}
