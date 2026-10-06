/*
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "utils/logger.h"

#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/io/dirent.h>

#include <stdarg.h>
#include <stdbool.h>
#include <stdatomic.h>

#define COLOR_RED    "\x1B[38;5;196m"
#define COLOR_PINK   "\x1B[38;5;212m"
#define COLOR_ORANGE "\x1B[38;5;202m"
#define COLOR_BLUE   "\x1B[38;5;32m"
#define COLOR_GREEN  "\x1B[32m"
#define COLOR_CYAN   "\x1B[36m"

#define COLOR_END    "\033[0m"

static SceKernelLwMutexWork _log_mutex;
static atomic_bool _log_mutex_ready = ATOMIC_VAR_INIT(false);

// Buffer A is used to adjust the format string.
static char buffer_a[2048];
// Buffer B is used to compile the final log using the updated format string.
static char buffer_b[2048];
// Plain-text line for the log file (no ANSI colors).
static char buffer_f[2200];

/*
 * File sink: ux0:data/<game>/logs/log_NNN.log, one new file per boot, NNN =
 * highest existing number + 1. Written with sceIoWrite per line (no stdio
 * buffering), so everything up to a crash is on the memory card -- the
 * psp2dmp alone doesn't say what the game was doing.
 */
#define LOG_DIR DATA_PATH "logs"

static SceUID _log_fd = -1;
static bool _log_file_tried = false;

/*
 * File writes are buffered: one synchronous sceIoWrite per line cost
 * ~10-16 ms on the memory card (log_006: 886 lines between frame 1 and 30 in
 * 14.2 s), i.e. logging alone held the game at ~2 FPS. Lines are queued in
 * RAM and written when the buffer is nearly full, every LOG_FLUSH_US, or
 * right away for warn/error/fatal/success, so a crash loses at most
 * LOG_FLUSH_US of info/debug lines and never the warning/error that
 * explains it.
 */
#define LOG_BUF_SIZE   (16 * 1024)
#define LOG_BUF_HIGH   (12 * 1024)
#define LOG_FLUSH_US   (250 * 1000)
static char _log_buf[LOG_BUF_SIZE];
static int _log_buf_len = 0;
static SceUInt64 _log_last_flush = 0;

static void _log_flush_locked(void) {
    if (_log_fd >= 0 && _log_buf_len > 0)
        sceIoWrite(_log_fd, _log_buf, _log_buf_len);
    _log_buf_len = 0;
    _log_last_flush = sceKernelGetProcessTimeWide();
}

void log_flush_if_due(void) {
    if (!atomic_load_explicit(&_log_mutex_ready, memory_order_relaxed) || _log_buf_len == 0)
        return;
    if (sceKernelGetProcessTimeWide() - _log_last_flush < LOG_FLUSH_US)
        return;
    sceKernelLockLwMutex(&_log_mutex, 1, NULL);
    _log_flush_locked();
    sceKernelUnlockLwMutex(&_log_mutex, 1);
}

static void _log_file_open(void) {
    _log_file_tried = true;
    sceIoMkdir(DATA_PATH, 0777);
    sceIoMkdir(LOG_DIR, 0777);

    int next = 1;
    SceUID dfd = sceIoDopen(LOG_DIR);
    if (dfd >= 0) {
        SceIoDirent ent;
        sceClibMemset(&ent, 0, sizeof(ent));
        while (sceIoDread(dfd, &ent) > 0) {
            const char *n = ent.d_name;
            if (n[0] == 'l' && n[1] == 'o' && n[2] == 'g' && n[3] == '_') {
                int v = 0, digits = 0;
                for (const char *c = n + 4; *c >= '0' && *c <= '9'; c++, digits++)
                    v = v * 10 + (*c - '0');
                if (digits > 0 && v >= next) next = v + 1;
            }
            sceClibMemset(&ent, 0, sizeof(ent));
        }
        sceIoDclose(dfd);
    }

    char path[256];
    sceClibSnprintf(path, sizeof(path), LOG_DIR "/log_%03d.log", next);
    _log_fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (_log_fd < 0) {
        sceClibPrintf("Error: could not open log file %s: 0x%x\n", path, _log_fd);
        return;
    }
    char hdr[320];
    int len = sceClibSnprintf(hdr, sizeof(hdr), "=== %s ===\n", path);
    sceIoWrite(_log_fd, hdr, len);
    sceClibPrintf("Logging to %s\n", path);
}

void _log_print(int t, const char* fmt, ...) {
    if (!atomic_load_explicit(&_log_mutex_ready, memory_order_relaxed)) {
        int ret = sceKernelCreateLwMutex(&_log_mutex, "log_lock", 0, 0, NULL);
        if (ret < 0) {
            sceClibPrintf("Error: failed to create log mutex: 0x%x\n", ret);
            return;
        }
        atomic_store_explicit(&_log_mutex_ready, true, memory_order_relaxed);
    }
    sceKernelLockLwMutex(&_log_mutex, 1, NULL);

    const char *color, *label;
    switch (t) {
        case LT_DEBUG:   color = COLOR_PINK;   label = "• debug";   break;
        case LT_INFO:    color = COLOR_BLUE;   label = "ℹ info";    break;
        case LT_WARN:    color = COLOR_ORANGE; label = "⚠ warning"; break;
        case LT_ERROR:   color = COLOR_RED;    label = "⨯ error";   break;
        case LT_FATAL:   color = COLOR_RED;    label = "! fatal";   break;
        case LT_SUCCESS: color = COLOR_GREEN;  label = "! success"; break;
        case LT_WAIT:    color = COLOR_CYAN;   label = "… waiting"; break;
        default:
            sceKernelUnlockLwMutex(&_log_mutex, 1);
            return;
    }

    va_list list;
    va_start(list, fmt);
    sceClibVsnprintf(buffer_a, sizeof(buffer_a), fmt, list);
    va_end(list);

    sceClibSnprintf(buffer_b, sizeof(buffer_b), " %s%-9s%s  %s\n",
                    color, label, COLOR_END, buffer_a);
    sceClibPrintf(buffer_b);

    if (!_log_file_tried) _log_file_open();
    if (_log_fd >= 0) {
        SceUInt64 ms = sceKernelGetProcessTimeWide() / 1000;
        int len = sceClibSnprintf(buffer_f, sizeof(buffer_f), "[%6u.%03u] %-9s  %s\n",
                                  (unsigned) (ms / 1000), (unsigned) (ms % 1000),
                                  label, buffer_a);
        if (len > (int) sizeof(buffer_f) - 1) len = sizeof(buffer_f) - 1;
        if (_log_buf_len + len > LOG_BUF_SIZE) _log_flush_locked();
        sceClibMemcpy(_log_buf + _log_buf_len, buffer_f, len);
        _log_buf_len += len;
        // warnings too: log_007's key line (a failed fopen just before the
        // crash) was still in RAM when the dump was taken.
        if (t == LT_WARN || t == LT_ERROR || t == LT_FATAL || t == LT_SUCCESS ||
            _log_buf_len >= LOG_BUF_HIGH ||
            sceKernelGetProcessTimeWide() - _log_last_flush >= LOG_FLUSH_US)
            _log_flush_locked();
    }

    sceKernelUnlockLwMutex(&_log_mutex, 1);
}
