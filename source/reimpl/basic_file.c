/*
 * basic_file.c -- stdio-backed std::__basic_file<char> + glf pread path.
 *
 * libDH3.so's libstdc++ (GNU, built against Bionic) inlines
 * __basic_file<char>::fd() as `ldrsh r0, [FILE, #14]`: Bionic's BSD FILE
 * layout (_p,_r,_w,_flags,_file). The FILE* comes from our fopen, which with
 * USE_SCELIBC_IO is a SceLibc FILE with a different layout -> garbage fd ->
 * newlib lseek/read/write on it (psp2dmp 2026-10-02 log_004: data abort in
 * _lseek_r <- __vita_fd_grab, from basic_filebuf::_M_seek -> seekoff).
 *
 * Every method that derives an fd is replaced by stdio on the same FILE*, so
 * all access goes through one buffered stream. Same for
 * glf::FileStream::Impl::DiskFileStreamImpl::ReadWithOffset, which does
 * pread(FILE->_file, ...). Sweep of `ldrsh ..., #14]` over the whole .so:
 * the remaining hits are unrelated structs (FreeType, g72x, input) plus
 * curl's file:// and OpenSSL's console, both unused on Vita.
 *
 * The functions are local symbols (not in dynsym): hooked by address, each
 * guarded by its first instruction word.
 */

#include <so_util/so_util.h>
#include <psp2/kernel/threadmgr.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>

#include "utils/logger.h"

// Through vfile.c's dispatchers: the FILE* may be a virtual OBB handle.
#include "reimpl/vfile.h"
#define S_FREAD   vf_fread
#define S_FWRITE  vf_fwrite
#define S_FSEEK   vf_fseek
#define S_FTELL   vf_ftell
#define S_FERROR  vf_ferror
#define S_FILENO  vf_fileno

extern so_module so_mod;

/* libstdc++ basic_file_stdio.h: { __c_file* _M_cfile; bool _M_cfile_created; } */
typedef struct {
    FILE *cfile;
    char created;
} basic_file;

static int bf_fd(basic_file *b) {
    return b->cfile ? S_FILENO(b->cfile) : -1;
}

static int bf_xsgetn(basic_file *b, char *s, int n) {
    if (n <= 0) return 0;
    size_t r = S_FREAD(s, 1, (size_t) n, b->cfile);
    if (r == 0 && S_FERROR(b->cfile)) return -1;
    return (int) r;
}

static int bf_xsputn(basic_file *b, const char *s, int n) {
    if (n <= 0) return 0;
    return (int) S_FWRITE(s, 1, (size_t) n, b->cfile);
}

static int bf_xsputn_2(basic_file *b, const char *s1, int n1,
                       const char *s2, int n2) {
    int r = bf_xsputn(b, s1, n1);
    if (r < n1) return r;
    return r + bf_xsputn(b, s2, n2);
}

/* streamoff seekoff(streamoff, ios_base::seekdir): beg/cur/end = 0/1/2,
 * same values as SEEK_SET/CUR/END. Original also rejects |off| > LONG_MAX. */
static long long bf_seekoff(basic_file *b, long long off, int way) {
    if (off > LONG_MAX || off < LONG_MIN) return -1;
    if (S_FSEEK(b->cfile, (long) off, way) != 0) return -1;
    return (long long) S_FTELL(b->cfile);
}

/* Bytes readable without blocking: for a regular file, size - position. */
static int bf_showmanyc(basic_file *b) {
    long cur = S_FTELL(b->cfile);
    if (cur < 0 || S_FSEEK(b->cfile, 0, SEEK_END) != 0) return -1;
    long end = S_FTELL(b->cfile);
    S_FSEEK(b->cfile, cur, SEEK_SET);
    return end > cur ? (int) (end - cur) : 0;
}

/* pread() is positional and atomic; emulate with seek/read/seek-back under a
 * lock so streaming threads sharing a FILE don't interleave. */
static SceUID pread_lock = -1;

static int glf_ReadWithOffset(void *self, void *buf, unsigned len, unsigned off) {
    // [vtable+36] = size; FILE* at this+8 (DiskFileStreamImpl, 0x842b88)
    unsigned (*get_size)(void *) = (*(unsigned (***)(void *)) self)[9];
    unsigned size = get_size(self);
    FILE *f = *(FILE **) ((uintptr_t) self + 8);
    unsigned n = size - off;          // unsigned, like the original rsb/cmp
    if (n > len) n = len;

    sceKernelLockMutex(pread_lock, 1, NULL);
    long pos = S_FTELL(f);
    int r = -1;
    if (S_FSEEK(f, (long) off, SEEK_SET) == 0) {
        r = (int) S_FREAD(buf, 1, n, f);
        if (r == 0 && n > 0 && S_FERROR(f)) r = -1;
    }
    if (pos >= 0) S_FSEEK(f, pos, SEEK_SET);
    sceKernelUnlockMutex(pread_lock, 1);

    if (r < 0) l_warn("[glf] ReadWithOffset(off=%u len=%u) failed", off, n);
    return r;
}

static const struct {
    uint32_t off, first_word;
    void *dst;
    const char *name;
} hooks[] = {
    { 0xd90ee0, 0xe5903000, bf_fd,        "__basic_file::fd" },
    { 0xd90f80, 0xe92d40f8, bf_xsgetn,    "__basic_file::xsgetn" },
    { 0xd90fd0, 0xe5903000, bf_xsputn,    "__basic_file::xsputn" },
    { 0xd90fdc, 0xe92d47f0, bf_xsputn_2,  "__basic_file::xsputn_2" },
    { 0xd91078, 0xe92d4038, bf_seekoff,   "__basic_file::seekoff" },
    { 0xd910f8, 0xe92d4030, bf_showmanyc, "__basic_file::showmanyc" },
    { 0x842b88, 0xe92d40f8, glf_ReadWithOffset, "DiskFileStreamImpl::ReadWithOffset" },
};

void patch_basic_file(void) {
    pread_lock = sceKernelCreateMutex("dh3_pread", 0, 0, NULL);
    int ok = 0, n = (int) (sizeof(hooks) / sizeof(hooks[0]));
    for (int i = 0; i < n; i++) {
        uintptr_t addr = so_mod.text_base + hooks[i].off;
        if (*(uint32_t *) addr != hooks[i].first_word) {
            l_error("FILE hook %s: unexpected code at 0x%x, skipped.",
                    hooks[i].name, hooks[i].off);
            continue;
        }
        hook_arm(addr, (uintptr_t) hooks[i].dst);
        ok++;
    }
    if (ok == n) l_success("Hooked %d FILE->_file users (basic_file/glf) onto stdio.", ok);
}
