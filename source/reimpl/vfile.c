/*
 * vfile.c -- virtual read-only FILE handles for the OBB, plus the stdio
 * dispatchers the .so's FILE* imports go through.
 *
 * libDH3 opens the OBB once per pak asset (CCustomPakReader::openFile(int)
 * -> createReadFile(obb path): 787 fopens in log_008) and every streaming
 * reader keeps its own copy open. SceLibc has a fixed FILE table: at 60
 * live FILEs (59 of them the OBB) the next fopen returned NULL, the
 * CGlfReadFile was left without a stream and CLimitReadFile::seek ->
 * CGlfReadFile::getPos dereferenced it (psp2dmp 2026-10-02 log_008).
 * Android has ~1024 fds, so the engine never cared.
 *
 * Read-only opens of *.obb now return a handle from a static pool:
 * one shared sceIoOpen fd per path, read with sceIoPread (positional,
 * thread-safe), each handle with its own position, eof/error flags and a
 * small read-ahead buffer. Handles are recognised by address (pool range),
 * never by in-band data. Every other FILE* goes to the real stdio
 * (SceLibc bridge, or newlib without USE_SCELIBC_IO) unchanged.
 *
 * Also fixes rewind/fseeko/ftello, which dynlib mapped to newlib even with
 * SceLibc FILEs.
 */

#include "reimpl/vfile.h"

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "utils/logger.h"

#ifdef USE_SCELIBC_IO
#include <libc_bridge/libc_bridge.h>
#define R_FOPEN    sceLibcBridge_fopen
#define R_FCLOSE   sceLibcBridge_fclose
#define R_FREAD    sceLibcBridge_fread
#define R_FWRITE   sceLibcBridge_fwrite
#define R_FSEEK    sceLibcBridge_fseek
#define R_FTELL    sceLibcBridge_ftell
#define R_FEOF     sceLibcBridge_feof
#define R_FERROR   sceLibcBridge_ferror
#define R_FFLUSH   sceLibcBridge_fflush
#define R_FGETS    sceLibcBridge_fgets
#define R_FGETC    sceLibcBridge_fgetc
#define R_GETC     sceLibcBridge_getc
#define R_UNGETC   sceLibcBridge_ungetc
#define R_SETVBUF  sceLibcBridge_setvbuf
#define R_FILENO   sceLibcBridge_fileno
#define R_FPUTC    sceLibcBridge_fputc
#define R_PUTC     sceLibcBridge_putc
#define R_FPUTS    sceLibcBridge_fputs
#define R_VFPRINTF sceLibcBridge_vfprintf
#else
#include <stdio.h>
#define R_FOPEN    fopen
#define R_FCLOSE   fclose
#define R_FREAD    fread
#define R_FWRITE   fwrite
#define R_FSEEK    fseek
#define R_FTELL    ftell
#define R_FEOF     feof
#define R_FERROR   ferror
#define R_FFLUSH   fflush
#define R_FGETS    fgets
#define R_FGETC    fgetc
#define R_GETC     getc
#define R_UNGETC   ungetc
#define R_SETVBUF  setvbuf
#define R_FILENO   fileno
#define R_FPUTC    fputc
#define R_PUTC     putc
#define R_FPUTS    fputs
#define R_VFPRINTF vfprintf
#endif

#define VF_MAX     512
#define VF_SHARED  8
#define VF_BUF     (16 * 1024)

typedef struct {
    char path[256];
    SceUID fd;
    int64_t size;
    int refs;
} vf_shared;

typedef struct {
    vf_shared *sh;        // NULL = free slot
    int64_t pos;
    int eof, err;
    uint8_t *buf;
    int64_t buf_pos;
    int buf_len;
} vfile;

static vfile pool[VF_MAX];
static vf_shared shared[VF_SHARED];
static SceKernelLwMutexWork vf_lock;
static int vf_lock_ready = 0;
static int vf_live = 0, vf_peak = 0;

static inline vfile *as_vf(FILE *f) {
    uintptr_t p = (uintptr_t) f;
    if (p < (uintptr_t) pool || p >= (uintptr_t) (pool + VF_MAX)) return NULL;
    return (vfile *) f;
}

int vf_is_virtual(FILE *f) {
    return as_vf(f) != NULL;
}

static void lock(void) {
    if (!vf_lock_ready) {   // first use is single-threaded (boot)
        sceKernelCreateLwMutex(&vf_lock, "vfile", 0, 0, NULL);
        vf_lock_ready = 1;
    }
    sceKernelLockLwMutex(&vf_lock, 1, NULL);
}

static void unlock(void) {
    sceKernelUnlockLwMutex(&vf_lock, 1);
}

static int wants_virtual(const char *path, const char *mode) {
    if (!path || !mode || mode[0] != 'r' || strchr(mode, '+')) return 0;
    size_t n = strlen(path);
    return n > 4 && strcasecmp(path + n - 4, ".obb") == 0;
}

FILE *vf_fopen(const char *path, const char *mode) {
    if (!wants_virtual(path, mode)) return R_FOPEN(path, mode);

    lock();
    vf_shared *sh = NULL;
    for (int i = 0; i < VF_SHARED; i++)
        if (shared[i].refs > 0 && strcmp(shared[i].path, path) == 0) { sh = &shared[i]; break; }
    if (!sh) {
        for (int i = 0; i < VF_SHARED; i++) if (shared[i].refs == 0) { sh = &shared[i]; break; }
        if (sh) {
            SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
            if (fd < 0) {
                unlock();
                return NULL;   // same as a failed fopen
            }
            snprintf(sh->path, sizeof(sh->path), "%s", path);
            sh->fd = fd;
            sh->size = sceIoLseek(fd, 0, SCE_SEEK_END);
        }
    }
    vfile *v = NULL;
    if (sh) {
        for (int i = 0; i < VF_MAX; i++) if (!pool[i].sh) { v = &pool[i]; break; }
    }
    if (!v) {
        if (sh && sh->refs == 0) sceIoClose(sh->fd);
        unlock();
        l_error("[vfile] pool exhausted (%d live), falling back to fopen(%s)", vf_live, path);
        return R_FOPEN(path, mode);
    }
    sh->refs++;
    memset(v, 0, sizeof(*v));
    v->sh = sh;
    if (++vf_live > vf_peak) {
        vf_peak = vf_live;
        if (vf_peak % 64 == 0) l_info("[vfile] %d OBB handles live", vf_peak);
    }
    unlock();
    return (FILE *) v;
}

static int vf_close(vfile *v) {
    lock();
    free(v->buf);
    vf_shared *sh = v->sh;
    if (sh && --sh->refs == 0) {
        sceIoClose(sh->fd);
        sh->path[0] = '\0';
    }
    memset(v, 0, sizeof(*v));
    vf_live--;
    unlock();
    return 0;
}

static size_t vf_read(vfile *v, void *dst, size_t n) {
    uint8_t *out = (uint8_t *) dst;
    size_t total = 0;
    while (n > 0) {
        if (v->buf_len > 0 && v->pos >= v->buf_pos && v->pos < v->buf_pos + v->buf_len) {
            size_t off = (size_t) (v->pos - v->buf_pos);
            size_t k = (size_t) v->buf_len - off;
            if (k > n) k = n;
            memcpy(out, v->buf + off, k);
            out += k; total += k; n -= k; v->pos += k;
            continue;
        }
        if (v->pos >= v->sh->size) break;
        if (n >= VF_BUF) {
            int r = sceIoPread(v->sh->fd, out, n, v->pos);
            if (r <= 0) { if (r < 0) v->err = 1; break; }
            out += r; total += r; n -= r; v->pos += r;
            continue;
        }
        if (!v->buf && !(v->buf = malloc(VF_BUF))) { v->err = 1; break; }
        int r = sceIoPread(v->sh->fd, v->buf, VF_BUF, v->pos);
        if (r <= 0) { v->buf_len = 0; if (r < 0) v->err = 1; break; }
        v->buf_pos = v->pos;
        v->buf_len = r;
    }
    if (n > 0 && !v->err) v->eof = 1;
    return total;
}

static int vf_seek(vfile *v, int64_t off, int whence) {
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? v->pos :
                   whence == SEEK_END ? v->sh->size : -1;
    if (base < 0 || base + off < 0) return -1;
    v->pos = base + off;
    v->eof = 0;
    return 0;
}

/* ---- dispatchers (dynlib.c points the .so's stdio imports here) ---- */

int vf_fclose(FILE *f) {
    vfile *v = as_vf(f);
    return v ? vf_close(v) : R_FCLOSE(f);
}

size_t vf_fread(void *ptr, size_t size, size_t count, FILE *f) {
    vfile *v = as_vf(f);
    if (!v) return R_FREAD(ptr, size, count, f);
    if (!size || !count) return 0;
    return vf_read(v, ptr, size * count) / size;
}

size_t vf_fwrite(const void *ptr, size_t size, size_t count, FILE *f) {
    vfile *v = as_vf(f);
    if (!v) return R_FWRITE(ptr, size, count, f);
    v->err = 1;
    return 0;
}

int vf_fseek(FILE *f, long off, int whence) {
    vfile *v = as_vf(f);
    return v ? vf_seek(v, off, whence) : R_FSEEK(f, off, whence);
}

long vf_ftell(FILE *f) {
    vfile *v = as_vf(f);
    return v ? (long) v->pos : R_FTELL(f);
}

int vf_fseeko(FILE *f, off_t off, int whence) {
    return vf_fseek(f, (long) off, whence);
}

off_t vf_ftello(FILE *f) {
    return (off_t) vf_ftell(f);
}

void vf_rewind(FILE *f) {
    vfile *v = as_vf(f);
    if (v) {
        vf_seek(v, 0, SEEK_SET);
        v->err = 0;
    } else {
        R_FSEEK(f, 0, SEEK_SET);   // rewind() also clears the error flag;
    }                              // SceLibc has no clearerr export
}

int vf_feof(FILE *f) {
    vfile *v = as_vf(f);
    return v ? v->eof : R_FEOF(f);
}

int vf_ferror(FILE *f) {
    vfile *v = as_vf(f);
    return v ? v->err : R_FERROR(f);
}

int vf_fflush(FILE *f) {
    vfile *v = as_vf(f);
    return v ? 0 : R_FFLUSH(f);
}

int vf_setvbuf(FILE *f, char *buf, int mode, size_t size) {
    vfile *v = as_vf(f);
    return v ? 0 : R_SETVBUF(f, buf, mode, size);
}

int vf_fileno(FILE *f) {
    vfile *v = as_vf(f);
    return v ? -1 : R_FILENO(f);   // no POSIX fd behind a virtual handle
}

int vf_fgetc(FILE *f) {
    vfile *v = as_vf(f);
    if (!v) return R_FGETC(f);
    unsigned char c;
    return vf_read(v, &c, 1) == 1 ? c : EOF;
}

int vf_getc(FILE *f) {
    vfile *v = as_vf(f);
    if (!v) return R_GETC(f);
    return vf_fgetc(f);
}

int vf_ungetc(int c, FILE *f) {
    vfile *v = as_vf(f);
    if (!v) return R_UNGETC(c, f);
    if (c == EOF || v->pos == 0) return EOF;
    v->pos--;          // read-only: pushing back the byte just read
    v->eof = 0;
    return c;
}

char *vf_fgets(char *s, int n, FILE *f) {
    vfile *v = as_vf(f);
    if (!v) return R_FGETS(s, n, f);
    if (n <= 0) return NULL;
    int i = 0;
    while (i < n - 1) {
        unsigned char c;
        if (vf_read(v, &c, 1) != 1) break;
        s[i++] = (char) c;
        if (c == '\n') break;
    }
    if (i == 0) return NULL;
    s[i] = '\0';
    return s;
}

int vf_fputc(int c, FILE *f) {
    vfile *v = as_vf(f);
    if (!v) return R_FPUTC(c, f);
    v->err = 1;
    return EOF;
}

int vf_putc(int c, FILE *f) {
    vfile *v = as_vf(f);
    if (!v) return R_PUTC(c, f);
    v->err = 1;
    return EOF;
}

int vf_fputs(const char *s, FILE *f) {
    vfile *v = as_vf(f);
    if (!v) return R_FPUTS(s, f);
    v->err = 1;
    return EOF;
}

int vf_vfprintf(FILE *f, const char *fmt, va_list ap) {
    vfile *v = as_vf(f);
    if (!v) return R_VFPRINTF(f, fmt, ap);
    v->err = 1;
    return -1;
}

int vf_fprintf(FILE *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vf_vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}
