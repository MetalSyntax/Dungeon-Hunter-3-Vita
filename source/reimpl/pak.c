/*
 * pak.c -- fixes in libDH3's OBB pak lookup and collada on-demand streaming.
 *
 * CCustomPakReader::findFile (0x67e6d8): binary search over the pak's flat,
 * sorted entry list (names have no directories). With ignorePaths (+301) it
 * keeps only what follows the last '/' -- except when the only '/' is at
 * index 0: `cmp r4,r7; addne r7,#1` never skips a leading slash. Collada
 * re-opens a streamed .bdae as dirname(name) + "/" + name, and a pak file's
 * IReadFile name is the bare entry name (openFile(int) ->
 * createLimitReadFile(entry->name)), so the request is
 * "/warlord_male_menu_idle.bdae": not found in the pak, the disk fallback
 * fopen()s "/warlord_male_menu_idle.bdae", fails, and the COnDemandReader is
 * built around a NULL file -> data abort in COnDemandReader::read
 * (psp2dmp 2026-10-02 log_007; name recovered from the dump's stack, the
 * log line itself was still in the logger's RAM buffer).
 *
 * The replacement keeps the original semantics (flags, lowercase copy,
 * lower_bound + equality) and adds: strip a leading separator too, and if
 * the full name misses, retry with its basename (the pak is flat).
 *
 * COnDemandReader::read (both overloads, identical bodies): guarded so a
 * reader without a file logs once and returns zeroed data instead of
 * dereferencing NULL.
 */

#include <so_util/so_util.h>
#include <kubridge.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "utils/logger.h"

extern so_module so_mod;

typedef struct {
    uint32_t offset;
    uint32_t size;
    const char *name;
    uint32_t flags;
} pak_entry;

#define PAK_ENTRIES_BEGIN 288
#define PAK_ENTRIES_END   292
#define PAK_IGNORE_CASE   300
#define PAK_IGNORE_PATHS  301

static int pak_search(const pak_entry *b, const pak_entry *e, const char *q) {
    const pak_entry *lo = b;
    int n = (int) (e - b);
    while (n > 0) {              // lower_bound, same as the original
        int half = n >> 1;
        if (strcmp(lo[half].name, q) < 0) {
            lo += half + 1;
            n -= half + 1;
        } else {
            n = half;
        }
    }
    if (lo != e && strcmp(lo->name, q) == 0) return (int) (lo - b);
    return -1;
}

static const char *after_last_sep(const char *s) {
    const char *r = s;
    for (const char *p = s; *p; p++)
        if (*p == '/' || *p == '\\') r = p + 1;
    return r;
}

static int pak_findFile(void *self, const char *name) {
    if (!name) return -1;
    const uint8_t *o = (const uint8_t *) self;
    const pak_entry *b = *(pak_entry * const *) (o + PAK_ENTRIES_BEGIN);
    const pak_entry *e = *(pak_entry * const *) (o + PAK_ENTRIES_END);
    if (b == e) return -1;

    char low[512];
    const char *q = name;
    if (o[PAK_IGNORE_CASE]) {
        size_t i = 0;
        for (; name[i] && i < sizeof(low) - 1; i++)
            low[i] = (name[i] >= 'A' && name[i] <= 'Z') ? name[i] + 32 : name[i];
        low[i] = '\0';
        q = low;
    }
    if (o[PAK_IGNORE_PATHS]) q = after_last_sep(q);

    int idx = pak_search(b, e, q);
    if (idx < 0) {
        const char *base = after_last_sep(q);
        if (base != q && *base) {
            idx = pak_search(b, e, base);
            if (idx >= 0) {
                static unsigned n = 0;
                if (n++ < 20) l_warn("[pak] '%s' found by basename '%s'", name, base);
            }
        }
    }
    return idx;
}

static int ondemand_read(void *self, int size, int pos, void *buf) {
    void **file = *(void ***) ((uintptr_t) self + 4);
    if (!file) {
        static unsigned n = 0;
        if (n++ < 20) l_error("[collada] COnDemandReader::read with no file (pos=%d size=%d): zero-filled", pos, size);
        if (buf && size > 0) memset(buf, 0, (size_t) size);
        return 0;
    }
    void **vt = *(void ***) file;
    ((int (*)(void *, int, int)) vt[6])(file, pos, 0);          // seek(pos, relative=false)
    return ((int (*)(void *, void *, int)) vt[3])(file, buf, size); // read(buf, size)
}

/*
 * Streamed .bdae name, pinned.
 * postLoadProcess (0x8f3d7c..0x8f3d90) does
 *     [SCollada + 4] = new core::string(readFile->getFullPath())
 * and CSkinnedMesh / CMesh / CAnimationBlock / createGeometry later re-open
 * the on-demand file by that string. It used to be shader text: not a freed
 * string but glf's never-written GetPath() buffer, see patch_glf_long_path.
 * We still keep our own copy: a libstdc++ COW rep (length, capacity,
 * refcount, chars) whose refcount never reaches -1, so no copy, assignment
 * or destructor of it can ever free it. One leaked string per streamed file.
 */
static int name_is_bad(const char *s) {
    return strlen(s) > 255 || strchr(s, '\n') != NULL;
}

void *dh3_pin_name(void *holder, void *scollada, const char *src) {
    static unsigned logged = 0;
    const char *cur = holder ? *(const char **) holder : NULL;
    if (!cur) return holder;
    size_t n = strlen(cur);
    uint32_t *rep = malloc(12 + n + 1);
    void **pinned = malloc(sizeof(void *));
    if (!rep || !pinned) {
        free(rep);
        free(pinned);
        return holder;
    }
    rep[0] = n;            // _M_length
    rep[1] = n;            // _M_capacity
    rep[2] = 0x3FFFFFFF;   // _M_refcount: shared "forever"
    memcpy(rep + 3, cur, n + 1);
    *pinned = rep + 3;

    if (name_is_bad(cur) || (src && strcmp(cur, src) != 0))
        l_error("[collada] streamed name already bad at load (SCollada=%p): %.80s", scollada, cur);
    else if (logged++ < 48)
        l_debug("[collada] streamed name pinned: SCollada=%p '%s'", scollada, cur);
    return pinned;
}

#define PIN_SITE  0x8f3d90u   // str r5, [r4, #4]
#define PIN_FIRST 0xE5845004u
#define ARM_B(from, to) (0xEA000000u | ((((int32_t)(to) - (int32_t)(from) - 8) >> 2) & 0x00FFFFFFu))

uintptr_t so_alloc_arena(so_module *so, uintptr_t range, uintptr_t dst, size_t sz);

static void patch_pin_name(void) {
    uintptr_t site = so_mod.text_base + PIN_SITE;
    if (*(uint32_t *) site != PIN_FIRST) {
        l_error("[collada] pin name: unexpected code 0x%08x at 0x%x, skipped.", *(uint32_t *) site, PIN_SITE);
        return;
    }
    uintptr_t tramp = so_alloc_arena(&so_mod, (1 << 24) - 1, site + 8, 11 * 4);
    if (!tramp) {
        l_error("[collada] pin name: arena full, skipped.");
        return;
    }
    uint32_t code[11];
    code[0]  = 0xE92D500Fu;   // push {r0-r3, ip, lr}
    code[1]  = 0xE1A00005u;   // mov  r0, r5   (engine's new'd string)
    code[2]  = 0xE1A01004u;   // mov  r1, r4   (SCollada)
    code[3]  = 0xE1A02006u;   // mov  r2, r6   (getFileName() it was built from)
    code[4]  = 0xE59FC010u;   // ldr  ip, [pc, #16] -> code[10]
    code[5]  = 0xE12FFF3Cu;   // blx  ip
    code[6]  = 0xE1A05000u;   // mov  r5, r0   (pinned copy)
    code[7]  = 0xE8BD500Fu;   // pop  {r0-r3, ip, lr}
    code[8]  = PIN_FIRST;     // str  r5, [r4, #4]
    code[9]  = ARM_B(tramp + 36, site + 4);
    code[10] = (uint32_t) (uintptr_t) dh3_pin_name;
    uint32_t jump = ARM_B(site, tramp);
    kuKernelCpuUnrestrictedMemcpy((void *) tramp, code, sizeof(code));
    kuKernelCpuUnrestrictedMemcpy((void *) site, &jump, sizeof(jump));
    l_success("Pinned streamed .bdae names in postLoadProcess (no more recycled-name re-opens).");
}

/*
 * constructGeometry(sret, db, driver, const char *id) guard. CSkinnedMesh::
 * instanciateMesh passes id = skin->sourceUrl + 1 (skips the '#'); with a
 * zero-filled skin that is (char *) 1 and the table scan strcmp()s it
 * (psp2dmp 1791162883). An id below 64 KB can't be a string: take the
 * engine's own "not found" exit (*sret = NULL), which instanciateMesh
 * already handles (beq 0x882f30).
 */
#define GEOM_SITE  0x8e9c9cu   // constructGeometry:   push {r4-sl, lr}
#define GEOM_FIRST 0xE92D47F0u
#define CTRL_SITE  0x8ece70u   // constructController: push {r4-fp, lr}
#define CTRL_FIRST 0xE92D4FF0u

static int guard_id_arg(uint32_t off, uint32_t first, const char *what) {
    uintptr_t site = so_mod.text_base + off;
    if (*(uint32_t *) site != first) {
        l_error("[collada] %s guard: unexpected code 0x%08x, skipped.", what, *(uint32_t *) site);
        return 0;
    }
    uintptr_t tramp = so_alloc_arena(&so_mod, (1 << 24) - 1, site + 8, 6 * 4);
    if (!tramp) {
        l_error("[collada] %s guard: arena full, skipped.", what);
        return 0;
    }
    uint32_t code[6];
    code[0] = 0xE3530801u;   // cmp   r3, #0x10000
    code[1] = 0x33A0C000u;   // movlo ip, #0
    code[2] = 0x3580C000u;   // strlo ip, [r0]   (*sret = NULL)
    code[3] = 0x312FFF1Eu;   // bxlo  lr
    code[4] = first;         // original push
    code[5] = ARM_B(tramp + 20, site + 4);
    uint32_t jump = ARM_B(site, tramp);
    kuKernelCpuUnrestrictedMemcpy((void *) tramp, code, sizeof(code));
    kuKernelCpuUnrestrictedMemcpy((void *) site, &jump, sizeof(jump));
    return 1;
}

// constructController (0x8ece70) has the same shape and the same zero-skin
// caller (instanciateMesh): psp2dmp 1791164950, LR 0x8eced4, r1 = 0x1.
static void patch_geometry_guard(void) {
    int ok = guard_id_arg(GEOM_SITE, GEOM_FIRST, "constructGeometry");
    ok += guard_id_arg(CTRL_SITE, CTRL_FIRST, "constructController");
    if (ok == 2)
        l_success("Guarded CColladaDatabase::constructGeometry/constructController against non-string ids.");
}

/*
 * ROOT CAUSE of the "shader text" streamed names (log_012..021, and the
 * 512-byte stack smash of log_009): glf::FileStream::Open (0x843070) copies
 * a .obb/.sav path into a stack buffer and, when it is longer than 80 chars
 * (`cmp sl, #80; bgt 0x84324c`), opens it from there and returns -- the path
 * buffer at FileStream+32 (what GetPath() 0x8441c0 returns) is never written.
 * FileStream is `new(0x574)`'d (CGlfReadFile ctor 0xa2af00), so GetPath()
 * returns whatever the heap block held before: on newlib, freed shader
 * source. CGlfReadFile copies it as its fullPath (+20), every pak
 * CLimitReadFile builds fullPath = parent->getFullPath() + "/" + name
 * (init 0xa5002c; the 1044-char, cap 0x804 string of log_021), and
 * postLoadProcess pins getFullPath() (vtable slot 12, not getFileName) as the
 * streamed .bdae name. Our OBB path is ~110 chars.
 * Fix: on that branch make FileStream+32 an empty string first. Android's
 * fresh heap made it "" too, giving "/name.bdae", which pak_findFile already
 * resolves (leading separator stripped).
 */
#define GLF_LONG_SITE  0x84324cu   // ldr r0, [r5, #1392]  (r5 = FileStream)
#define GLF_LONG_FIRST 0xE5950570u

static void patch_glf_long_path(void) {
    uintptr_t site = so_mod.text_base + GLF_LONG_SITE;
    if (*(uint32_t *) site != GLF_LONG_FIRST) {
        l_error("[glf] long-path fix: unexpected code 0x%08x, skipped.", *(uint32_t *) site);
        return;
    }
    uintptr_t tramp = so_alloc_arena(&so_mod, (1 << 24) - 1, site + 8, 4 * 4);
    if (!tramp) {
        l_error("[glf] long-path fix: arena full, skipped.");
        return;
    }
    uint32_t code[4];
    code[0] = 0xE3A03000u;      // mov  r3, #0       (r3 is reloaded right after)
    code[1] = 0xE5C53020u;      // strb r3, [r5, #32]
    code[2] = GLF_LONG_FIRST;   // ldr  r0, [r5, #1392]
    code[3] = ARM_B(tramp + 12, site + 4);
    uint32_t jump = ARM_B(site, tramp);
    kuKernelCpuUnrestrictedMemcpy((void *) tramp, code, sizeof(code));
    kuKernelCpuUnrestrictedMemcpy((void *) site, &jump, sizeof(jump));
    l_success("glf::FileStream::Open: long .obb/.sav paths get a valid GetPath() (no heap garbage).");
}

static const struct {
    uint32_t off, first_word;
    void *dst;
    const char *name;
} hooks[] = {
    { 0x67e6d8, 0xe92d4ff8, pak_findFile,  "CCustomPakReader::findFile" },
    { 0x8f23ec, 0xe92d4070, ondemand_read, "COnDemandReader::read" },
    { 0x8f2430, 0xe92d4070, ondemand_read, "COnDemandReader::read(cb)" },
};

void patch_pak(void) {
    int ok = 0, n = (int) (sizeof(hooks) / sizeof(hooks[0]));
    for (int i = 0; i < n; i++) {
        uintptr_t addr = so_mod.text_base + hooks[i].off;
        if (*(uint32_t *) addr != hooks[i].first_word) {
            l_error("pak hook %s: unexpected code at 0x%x, skipped.", hooks[i].name, hooks[i].off);
            continue;
        }
        hook_arm(addr, (uintptr_t) hooks[i].dst);
        ok++;
    }
    if (ok == n) l_success("Hooked pak findFile + COnDemandReader::read guards.");

    // CResFile::releaseObjects (0x8f2de8, only called from ~CResFile) deletes
    // the streamed-file name string at [SCollada + 4] that postLoadProcess
    // (0x8f3d7c) new'd, without clearing the pointer. A .bdae loaded twice
    // shares that SCollada slot, so the first ~CResFile frees the name the
    // survivor still uses: CSkinnedMesh then re-opens the on-demand file by a
    // freed string (log_012/013/016: the "name" is recycled shader source),
    // the open fails, COnDemandReader zero-fills the skin data and
    // constructGeometry strcmp()s a garbage id (psp2dmp 2026-10-04, PC strcmp,
    // LR constructGeometry 0x8e9cf0, r1 = 0x1). The data offsets are identical
    // across runs, so only the name dies. Skip the delete: `beq` -> `b`, a few
    // bytes leaked per unloaded streamed .bdae.
    patch_glf_long_path();
    patch_pin_name();
    patch_geometry_guard();

    uintptr_t rel = so_mod.text_base + 0x8f2e00;
    if (*(uint32_t *) rel == 0x0A000003u) {   // beq 8f2e14
        uint32_t b = 0xEA000003u;             // b   8f2e14
        kuKernelCpuUnrestrictedMemcpy((void *) rel, &b, sizeof(b));
        l_success("CResFile::releaseObjects keeps the streamed .bdae name (no use-after-free).");
    } else {
        l_error("CResFile::releaseObjects: unexpected code 0x%08x, name UAF fix skipped.", *(uint32_t *) rel);
    }
}
