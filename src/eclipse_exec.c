/*
 * eclipseexec — process state, error reporting, EGL preparation.
 * Copyright (c) 2026 Shadow. SPDX-License-Identifier: MIT
 */

#define ECLIPSE_LOG_TAG "EclipseExec"
#include "eclipseexec.h"
#include "eclipse_log.h"
#include "eclipse_namespace.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

/* ---- published state ---------------------------------------------------- */

eclipseexec_renderspec_t eclipseexec_renderspec;
const char *eclipseexec_native_dir = NULL;

/* ---- private state ------------------------------------------------------ */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * Configuration strings are published as raw pointers and read by SDL on its
 * own threads. Replacing one means another thread may be holding it right
 * now, so the previous value is never freed: a few dozen bytes for the life
 * of the process beats a use-after-free in the renderer.
 */
static char *g_native_dir;
static char *g_egl_path;
static bool g_egl_bypass;

/*
 * The last failure on this thread. Thread-local rather than locked: the
 * thread that fails is the thread that asks, so there is nothing to
 * synchronise and no torn read to worry about.
 */
static _Thread_local char g_error[512];

/* ---- diagnostics -------------------------------------------------------- */

void eclipseexec_set_error(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(g_error, sizeof(g_error), format, args);
    va_end(args);

    /* The message goes to logcat as well as the buffer: a failing launcher is
     * usually inspected through logcat first. */
    ECLIPSE_LOGE("%s", g_error);
}

const char *eclipseexec_last_error(void)
{
    return g_error;
}

void eclipseexec_clear_error(void)
{
    g_error[0] = '\0';
}

int eclipseexec_device_api_level(void)
{
#ifdef __ANDROID__
    static int cached = 0; /* benign: every thread reads the same system property */
    if (cached != 0) {
        return cached;
    }

    char value[PROP_VALUE_MAX];
    memset(value, 0, sizeof(value));
    if (__system_property_get("ro.build.version.sdk", value) <= 0) {
        return 0;
    }

    int level = atoi(value);
    if (level > 0) {
        cached = level;
    }
    return level;
#else
    /* No system properties off Android; 0 means "unknown" to callers. */
    return 0;
#endif
}

/* ---- configuration ------------------------------------------------------ */

bool eclipseexec_set_native_dir(const char *dir)
{
    if (dir == NULL || dir[0] == '\0') {
        eclipseexec_set_error("native library directory is null or empty");
        return false;
    }

    pthread_mutex_lock(&g_lock);
    if (g_native_dir != NULL && strcmp(g_native_dir, dir) == 0) {
        /* Already recorded: nothing to allocate, nothing to publish. */
        pthread_mutex_unlock(&g_lock);
        return true;
    }
    pthread_mutex_unlock(&g_lock);

    char *copy = strdup(dir);
    if (copy == NULL) {
        eclipseexec_set_error("out of memory recording the native library directory");
        return false;
    }

    pthread_mutex_lock(&g_lock);
    g_native_dir = copy;
    eclipseexec_native_dir = copy;
    pthread_mutex_unlock(&g_lock);

    ECLIPSE_LOGI("native library directory: %s", dir);
    return true;
}

void eclipseexec_set_display_params(int width, int height, float hz)
{
    pthread_mutex_lock(&g_lock);
    eclipseexec_renderspec.disp_width = width;
    eclipseexec_renderspec.disp_height = height;
    eclipseexec_renderspec.disp_hz = hz;
    pthread_mutex_unlock(&g_lock);

    ECLIPSE_LOGD("display %dx%d at %.2f Hz", width, height, (double)hz);
}

/* ---- EGL ---------------------------------------------------------------- */

bool eclipseexec_prepare_egl(const char *path, bool use_bypass, bool force_gles, int gles_major)
{
    if (path == NULL || path[0] == '\0') {
        eclipseexec_set_error("prepare_egl: no GL library path given");
        return false;
    }

    if (use_bypass) {
        const char *dir = eclipseexec_native_dir;
        if (dir == NULL || dir[0] == '\0') {
            eclipseexec_set_error("prepare_egl: namespace bypass requested before "
                                  "setNativeLibraryDir()");
            return false;
        }
        if (!eclipse_namespace_init(dir)) {
            /* eclipse_namespace_init() has already recorded why. */
            return false;
        }
    }

    char *copy = strdup(path);
    if (copy == NULL) {
        eclipseexec_set_error("prepare_egl: out of memory");
        return false;
    }

    pthread_mutex_lock(&g_lock);
    if (g_egl_path == NULL || strcmp(g_egl_path, path) != 0) {
        g_egl_path = copy; /* published, never freed */
        copy = NULL;
    }
    g_egl_bypass = use_bypass;
    eclipseexec_renderspec.force_gles_context = force_gles ? 1 : 0;
    eclipseexec_renderspec.override_major_version = gles_major;
    pthread_mutex_unlock(&g_lock);
    free(copy);

    if (eclipseexec_acq_egl_handle() == NULL) {
        return false;
    }

    ECLIPSE_LOGI("GL driver %s loaded (gles=%d, major=%d, bypass=%d)",
                 path, force_gles ? 1 : 0, gles_major, use_bypass ? 1 : 0);
    return true;
}

void *eclipseexec_acq_egl_handle(void)
{
    pthread_mutex_lock(&g_lock);
    const char *path = g_egl_path;
    bool bypass = g_egl_bypass;
    pthread_mutex_unlock(&g_lock);

    if (path == NULL) {
        eclipseexec_set_error("acq_egl_handle: prepare_egl() has not been called");
        return NULL;
    }

    void *handle;
    if (bypass) {
        /* The namespace code records the reason on failure and has already
         * consumed dlerror(), so this branch must not overwrite its message
         * with "unknown error". */
        handle = eclipse_namespace_dlopen(path, RTLD_LOCAL | RTLD_NOW);
        if (handle == NULL) {
            return NULL;
        }
    } else {
        handle = dlopen(path, RTLD_LOCAL | RTLD_NOW);
        if (handle == NULL) {
            const char *detail = dlerror();
            eclipseexec_set_error("cannot load %s: %s", path,
                                  detail ? detail : "unknown error");
            return NULL;
        }
    }

    eclipseexec_clear_error();
    return handle;
}
