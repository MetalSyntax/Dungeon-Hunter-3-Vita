/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/io.h"

#include <string.h>
#include <psp2/io/stat.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <stdlib.h>
#include <dirent.h>
#include <stdarg.h>
#include <psp2/kernel/threadmgr.h>

#ifdef USE_SCELIBC_IO
#include <libc_bridge/libc_bridge.h>
#endif

#include "utils/logger.h"
#include "utils/utils.h"
#include "reimpl/vfile.h"
#include <so_util/so_util.h>

// Includes the following inline utilities:
// int oflags_musl_to_newlib(int flags);
// dirent64_bionic * dirent_newlib_to_bionic(struct dirent* dirent_newlib);
// void stat_newlib_to_bionic(struct stat * src, stat64_bionic * dst);
#include "reimpl/bits/_struct_converters.c"

// Ported from the DH2 sibling port (same glitch engine; root-caused there via
// crash triage on hardware, log_066/067): glitch's COnDemandReader re-joins
// an already-absolute path onto the data dir, producing
// "ux0:data/<game>/ux0:data/<game>/data/...", whose open fails and leaves a
// NULL stream that the reader then dereferences. Restart any path at its
// LAST embedded Vita drive prefix ("/ux0:", "/app0:", ...).
static const char * collapse_doubled_root(const char * path) {
    const char * last = path;
    for (const char * p = path; p && *p; p++) {
        if (*p == '/' && p[1] && p != path) {
            const char * c = strchr(p + 1, ':');
            const char * slash = strchr(p + 1, '/');
            if (c && (!slash || c < slash) && c - (p + 1) <= 5) last = p + 1;
        }
    }
    if (last != path) l_debug("path with doubled root collapsed: %s -> %s", path, last);
    return last;
}

/*
 * Android's private app dir is hardcoded in libDH3.so ("/data/data/<pkg>",
 * 2 literals): the profile save (dh3_profile.savegame[.bak]) and
 * DebugSwitches.savegame live there, also tried as "./data/data/<pkg>/...".
 * Without this every save load/write failed (log_006: ~80 failed fopens).
 * Mapped onto the getFilesDir() equivalent java.c already creates.
 */
#define ANDROID_APP_DIR "/data/data/com.gameloft.android.ANMP.GloftD3HM"
#define VITA_APP_DIR    DATA_PATH "files"

const char * fix_path(const char * path, char * buf, size_t size) {
    if (!path) return path;
    path = collapse_doubled_root(path);
    const char * p = path;
    if (p[0] == '.' && p[1] == '/') p++;
    size_t n = sizeof(ANDROID_APP_DIR) - 1;
    if (strncmp(p, ANDROID_APP_DIR, n) == 0 && (p[n] == '/' || p[n] == '\0')) {
        snprintf(buf, size, VITA_APP_DIR "%s", p + n);
        return buf;
    }
    return path;
}

int mkdir_soloader(const char * path, mode_t mode) {
    char buf[512];
    return mkdir(fix_path(path, buf, sizeof(buf)), mode);
}

// Save bug (log_022): dh3_profile.savegame and dh3_000.savegame vanish
// between boots with nothing in the log. Every delete/rename the engine does
// is logged with its .so caller (deleteSavefile 0x60ea48, backupSavefile
// 0x60ead4, glf::Fs::RemoveFile, glot tracking) until that is pinned down.
extern so_module so_mod;

static uintptr_t so_caller(void *ra) {
    uintptr_t a = (uintptr_t) ra;
    return (a >= so_mod.text_base && a < so_mod.text_base + so_mod.text_size) ? a - so_mod.text_base : a;
}

int remove_soloader(const char * path) {
    char buf[512];
    const char *p = fix_path(path, buf, sizeof(buf));
    int r = remove(p);
    l_warn("remove(%s) = %d <- so+0x%x", p, r, so_caller(__builtin_return_address(0)));
    return r;
}

int unlink_soloader(const char * path) {
    char buf[512];
    const char *p = fix_path(path, buf, sizeof(buf));
    int r = unlink(p);
    l_warn("unlink(%s) = %d <- so+0x%x", p, r, so_caller(__builtin_return_address(0)));
    return r;
}

// POSIX/bionic rename() atomically replaces an existing target; Vita's
// sceIoRename fails instead, so drop the target first.
int rename_soloader(const char * from, const char * to) {
    char b1[512], b2[512];
    const char *f = fix_path(from, b1, sizeof(b1));
    const char *t = fix_path(to, b2, sizeof(b2));
    int r = rename(f, t);
    if (r != 0) {
        struct stat st;
        if (stat(t, &st) == 0 && stat(f, &st) == 0) {
            remove(t);
            r = rename(f, t);
        }
    }
    l_warn("rename(%s -> %s) = %d <- so+0x%x", f, t, r, so_caller(__builtin_return_address(0)));
    return r;
}

int chmod_soloader(const char * path, mode_t mode) {
    char buf[512];
    return chmod(fix_path(path, buf, sizeof(buf)), mode);
}

/*
 * In-game FPS dips (log_024: 18-21 fps for a few seconds while effects /
 * monsters stream in): the engine probes the disk before the OBB pak for
 * every asset, as "ux0:data/dungeonhunter3/data/assets/..." -- a directory
 * that doesn't exist on the Vita (everything is in the OBB). Each probe was
 * a memory-card syscall plus a log line. If DATA_PATH "data" is absent
 * (checked once, re-checked after any write under it), reads below it fail
 * immediately with ENOENT.
 */
#define DISK_DATA_DIR DATA_PATH "data"

static volatile int disk_data_state = -1;   // -1 unknown, 0 absent, 1 present

static int under_disk_data(const char * path) {
    size_t n = sizeof(DISK_DATA_DIR) - 1;
    return strncmp(path, DISK_DATA_DIR, n) == 0 && (path[n] == '/' || path[n] == '\0');
}

static int known_missing(const char * path) {
    if (!under_disk_data(path)) return 0;
    if (disk_data_state < 0) {
        SceIoStat st;
        disk_data_state = sceIoGetstat(DISK_DATA_DIR, &st) >= 0;
        l_info("[io] %s %s: disk probes below it %s.", DISK_DATA_DIR,
               disk_data_state ? "exists" : "absent", disk_data_state ? "go to the card" : "short-circuit");
    }
    if (disk_data_state) return 0;
    errno = ENOENT;
    return 1;
}

FILE * fopen_soloader(const char * filename, const char * mode) {
    char path_buf[512];
    filename = fix_path(filename, path_buf, sizeof(path_buf));
    if (strcmp(filename, "/proc/cpuinfo") == 0) {
        return fopen_soloader("app0:/cpuinfo", mode);
    } else if (strcmp(filename, "/proc/meminfo") == 0) {
        return fopen_soloader("app0:/meminfo", mode);
    }

    int reading = mode && mode[0] == 'r' && !strchr(mode, '+');
    if (reading && known_missing(filename)) return NULL;
    if (!reading && under_disk_data(filename)) disk_data_state = -1;

    // vf_fopen: read-only *.obb -> virtual handle (vfile.c), else real fopen.
    FILE* ret = vf_fopen(filename, mode);

    if (!ret)
        l_warn("fopen(%s, %s): %p", filename, mode, ret);
    else if (!vf_is_virtual(ret))   // OBB opens happen per asset: not logged
        l_debug("fopen(%s, %s): %p", filename, mode, ret);

    return ret;
}

int open_soloader(const char * path, int oflag, ...) {
    char path_buf[512];
    path = fix_path(path, path_buf, sizeof(path_buf));
    if (strcmp(path, "/proc/cpuinfo") == 0) {
        return open_soloader("app0:/cpuinfo", oflag);
    } else if (strcmp(path, "/proc/meminfo") == 0) {
        return open_soloader("app0:/meminfo", oflag);
    }

    mode_t mode = 0666;
    if (((oflag & BIONIC_O_CREAT) == BIONIC_O_CREAT) ||
        ((oflag & BIONIC_O_TMPFILE) == BIONIC_O_TMPFILE)) {
        va_list args;
        va_start(args, oflag);
        mode = (mode_t)(va_arg(args, int));
        va_end(args);
    }

    oflag = oflags_bionic_to_newlib(oflag);
    int ret = open(path, oflag, mode);
    if (ret >= 0)
        l_debug("open(%s, %x): %i", path, oflag, ret);
    else
        l_warn("open(%s, %x): %i", path, oflag, ret);
    return ret;
}

int fstat_soloader(int fd, stat64_bionic * buf) {
    struct stat st;
    int res = fstat(fd, &st);

    if (res == 0)
        stat_newlib_to_bionic(&st, buf);

    l_debug("fstat(%i): %i", fd, res);
    return res;
}

int stat_soloader(const char * path, stat64_bionic * buf) {
    char path_buf[512];
    path = fix_path(path, path_buf, sizeof(path_buf));
    if (known_missing(path)) return -1;
    struct stat st;
    int res = stat(path, &st);

    if (res == 0)
        stat_newlib_to_bionic(&st, buf);

    l_debug("stat(%s): %i", path, res);
    return res;
}

int fclose_soloader(FILE * f) {
    int virt = vf_is_virtual(f);
    int ret = vf_fclose(f);

    if (!virt) l_debug("fclose(%p): %i", f, ret);
    return ret;
}

int close_soloader(int fd) {
    int ret = close(fd);
    l_debug("close(%i): %i", fd, ret);
    return ret;
}

DIR* opendir_soloader(char* _pathname) {
    char path_buf[512];
    _pathname = (char *) fix_path(_pathname, path_buf, sizeof(path_buf));
    DIR* ret = opendir(_pathname);
    l_debug("opendir(\"%s\"): %p", _pathname, ret);
    return ret;
}

struct dirent64_bionic * readdir_soloader(DIR * dir) {
    static struct dirent64_bionic dirent_tmp;

    struct dirent* ret = readdir(dir);
    l_debug("readdir(%p): %p", dir, ret);

    if (ret) {
        dirent64_bionic* entry_tmp = dirent_newlib_to_bionic(ret);
        memcpy(&dirent_tmp, entry_tmp, sizeof(dirent64_bionic));
        free(entry_tmp);
        return &dirent_tmp;
    }

    return NULL;
}

int readdir_r_soloader(DIR * dirp, dirent64_bionic * entry,
                       dirent64_bionic ** result) {
    struct dirent dirent_tmp;
    struct dirent * pdirent_tmp;

    int ret = readdir_r(dirp, &dirent_tmp, &pdirent_tmp);

    if (ret == 0) {
        dirent64_bionic* entry_tmp = dirent_newlib_to_bionic(&dirent_tmp);
        memcpy(entry, entry_tmp, sizeof(dirent64_bionic));
        *result = (pdirent_tmp != NULL) ? entry : NULL;
        free(entry_tmp);
    }

    l_debug("readdir_r(%p, %p, %p): %i", dirp, entry, result, ret);
    return ret;
}

int closedir_soloader(DIR * dir) {
    int ret = closedir(dir);
    l_debug("closedir(%p): %i", dir, ret);
    return ret;
}

int fcntl_soloader(int fd, int cmd, ...) {
    l_warn("fcntl(%i, %i, ...): not implemented", fd, cmd);
    return 0;
}

int ioctl_soloader(int fd, int request, ...) {
    l_warn("ioctl(%i, %i, ...): not implemented", fd, request);
    return 0;
}

int fsync_soloader(int fd) {
    int ret = fsync(fd);
    l_debug("fsync(%i): %i", fd, ret);
    return ret;
}
