/*
 * FalsoJNI_Logger.h
 *
 * Fake Java Native Interface, providing JavaVM and JNIEnv objects.
 *
 * Copyright (C) 2022-2023 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#pragma clang diagnostic push
#pragma ide diagnostic ignored "bugprone-reserved-identifier"

#ifndef FALSOJNI_LOGGER_H
#define FALSOJNI_LOGGER_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define fjni_logv_info(fmt, ...)  _fjni_log_info(__FILE__, __LINE__, __func__, fmt, __VA_ARGS__)
#define fjni_logv_warn(fmt, ...)  _fjni_log_warn(__FILE__, __LINE__, __func__, fmt, __VA_ARGS__)
#define fjni_logv_dbg(fmt, ...)   _fjni_log_debug(__FILE__, __LINE__, __func__, fmt, __VA_ARGS__)
#define fjni_logv_err(fmt, ...)   _fjni_log_error(__FILE__, __LINE__, __func__, fmt, __VA_ARGS__)

#define fjni_log_info(fmt)        _fjni_log_info(__FILE__, __LINE__, __func__, fmt)
#define fjni_log_warn(fmt)        _fjni_log_warn(__FILE__, __LINE__, __func__, fmt)

/* [DH3] Per-call-site rate limit for warnings the engine triggers every
 * frame (unregistered method IDs, NULL strings): first 20, then 1 in 1000.
 * Unthrottled they were ~1600 log lines per run and a big part of the
 * per-frame cost (log_006). */
#define fjni_logv_warn_limited(fmt, ...) do { \
        static unsigned _fjni_n = 0; \
        if (_fjni_n < 20 || _fjni_n % 1000 == 0) \
            _fjni_log_warn(__FILE__, __LINE__, __func__, fmt " (x%u)", __VA_ARGS__, _fjni_n + 1); \
        _fjni_n++; \
    } while (0)
#define fjni_log_dbg(fmt)         _fjni_log_debug(__FILE__, __LINE__, __func__, fmt)
#define fjni_log_err(fmt)         _fjni_log_error(__FILE__, __LINE__, __func__, fmt)

void _fjni_log_info(const char *fi, int li, const char *fn, const char* fmt, ...);
void _fjni_log_warn(const char *fi, int li, const char *fn, const char* fmt, ...);
void _fjni_log_debug(const char *fi, int li, const char *fn, const char* fmt, ...);
void _fjni_log_error(const char *fi, int li, const char *fn, const char* fmt, ...);

#ifdef __cplusplus
};
#endif

#endif // FALSOJNI_LOGGER_H
