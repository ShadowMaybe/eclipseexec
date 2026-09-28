/*
 * eclipseexec — logging.
 * Copyright (c) 2026 Shadow. SPDX-License-Identifier: MIT
 *
 * printf goes nowhere useful on Android unless someone has redirected
 * stdout, so everything in the library logs through logcat. A file sets
 * ECLIPSE_LOG_TAG before including this to get a finer-grained tag:
 *
 *   #define ECLIPSE_LOG_TAG "EclipseExec/ns"
 *   #include "eclipse_log.h"
 *
 * Off Android — the host unit tests in tests/ — the same macros write to
 * stderr, so a failing test shows what the library said.
 */
#ifndef ECLIPSE_LOG_H
#define ECLIPSE_LOG_H

#ifndef ECLIPSE_LOG_TAG
#define ECLIPSE_LOG_TAG "EclipseExec"
#endif

#ifdef __ANDROID__

#include <android/log.h>

#define ECLIPSE_LOGV(...) __android_log_print(ANDROID_LOG_VERBOSE, ECLIPSE_LOG_TAG, __VA_ARGS__)
#define ECLIPSE_LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, ECLIPSE_LOG_TAG, __VA_ARGS__)
#define ECLIPSE_LOGI(...) __android_log_print(ANDROID_LOG_INFO, ECLIPSE_LOG_TAG, __VA_ARGS__)
#define ECLIPSE_LOGW(...) __android_log_print(ANDROID_LOG_WARN, ECLIPSE_LOG_TAG, __VA_ARGS__)
#define ECLIPSE_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, ECLIPSE_LOG_TAG, __VA_ARGS__)

#else /* !__ANDROID__ */

#include <stdio.h>

#define ECLIPSE_LOG_LINE(level, ...) \
    do { \
        fprintf(stderr, "%s/" ECLIPSE_LOG_TAG ": ", level); \
        fprintf(stderr, __VA_ARGS__); \
        fputc('\n', stderr); \
    } while (0)

#define ECLIPSE_LOGV(...) ECLIPSE_LOG_LINE("V", __VA_ARGS__)
#define ECLIPSE_LOGD(...) ECLIPSE_LOG_LINE("D", __VA_ARGS__)
#define ECLIPSE_LOGI(...) ECLIPSE_LOG_LINE("I", __VA_ARGS__)
#define ECLIPSE_LOGW(...) ECLIPSE_LOG_LINE("W", __VA_ARGS__)
#define ECLIPSE_LOGE(...) ECLIPSE_LOG_LINE("E", __VA_ARGS__)

#endif /* __ANDROID__ */

#endif /* ECLIPSE_LOG_H */
