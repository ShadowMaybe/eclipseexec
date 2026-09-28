/*
 * eclipseexec — Vulkan driver selection.
 * Copyright (c) 2026 Shadow. SPDX-License-Identifier: MIT
 *
 * Two ways to get a Vulkan loader, in the order the launcher asks for them:
 *
 *   Turnip   — the open-source Freedreno driver, shipped by the launcher in its
 *              own native library directory. Android will not let it be loaded
 *              from there, so it goes in through the private namespace, and the
 *              system loader is loaded beside it under a second SONAME with an
 *              interposer in front that answers "vulkan.*" ICD requests with
 *              the driver we already have.
 *   system   — libvulkan.so out of /system, the default whenever Turnip was not
 *              selected or is unavailable.
 *
 * The Turnip path exists on arm64-v8a only: that is the architecture Mesa's
 * Turnip driver is built for here, and the interposer that makes it work is
 * linked into libeclipsehook.so for that ABI alone.
 */

#define ECLIPSE_LOG_TAG "EclipseExec/vulkan"
#include "eclipseexec.h"
#include "eclipse_log.h"
#include "eclipse_namespace.h"

#include <android/dlext.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define ECLIPSE_SYSTEM_VULKAN "libvulkan.so"
#define ECLIPSE_TURNIP_DRIVER "libvulkan_freedreno.so"
#define ECLIPSE_TURNIP_HOOK   "libeclipsehook.so"

/*
 * The system loader is loaded under a second identity so it does not collide
 * with the Turnip driver. Exactly as long as "libvulkan.so" (12 characters),
 * because the DT_SONAME entry is rewritten in place.
 */
#define ECLIPSE_LOADER_ALIAS  "libclipse.so"

/* The Vulkan loader's HAL interface arrived in Android 9. */
#define ECLIPSE_MIN_TURPIP_API 28

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_turnip_selected;
#ifdef ECLIPSE_HAVE_TURNIP
/*
 * Serialises load_turnip() end to end. The ready flag is only ever consulted
 * once the attempt ahead of this one has finished, so two threads arriving
 * together load the driver, arm the hook and clone the system loader once
 * between them instead of doing all of it twice — which on the second pass
 * would arm the hook again, load a second patched copy and lose a descriptor.
 */
static pthread_mutex_t g_load_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_turnip_ready;
#endif

void eclipseexec_set_use_turnip(bool enable)
{
    pthread_mutex_lock(&g_lock);
    g_turnip_selected = enable;
    pthread_mutex_unlock(&g_lock);

    ECLIPSE_LOGI("Turnip %s", enable ? "selected" : "not selected");
}

#ifdef ECLIPSE_HAVE_TURNIP

/* Called with g_load_lock held. g_lock is taken only to read and publish the
 * two flags below, never across a load. */
static bool load_turnip_locked(void)
{
    pthread_mutex_lock(&g_lock);
    bool ready = g_turnip_ready;
    bool selected = g_turnip_selected;
    pthread_mutex_unlock(&g_lock);

    if (ready) {
        return true;
    }
    if (!selected) {
        eclipseexec_set_error("Turnip preload requested but Turnip was not selected");
        return false;
    }

    const char *dir = eclipseexec_native_dir;
    if (dir == NULL || dir[0] == '\0') {
        eclipseexec_set_error("Turnip needs setNativeLibraryDir() first");
        return false;
    }
    if (!eclipse_namespace_init(dir)) {
        return false;
    }

    /* Everything below loads into the driver namespace, in this order: the
     * interposer has to be there before the loader, or nothing it does gets
     * seen. */
    void *hook = eclipse_namespace_dlopen(ECLIPSE_TURNIP_HOOK, RTLD_LOCAL | RTLD_NOW);
    if (hook == NULL) {
        /* The namespace code consumed dlerror() and recorded the exact reason.
         * Asking dlerror() again here would return NULL and replace it with
         * "unknown error", which is the one message nobody can act on. */
        return false;
    }

    void *driver = eclipse_namespace_dlopen(ECLIPSE_TURNIP_DRIVER, RTLD_LOCAL | RTLD_NOW);
    if (driver == NULL) {
        return false; /* likewise: eclipse_namespace_dlopen() has said why */
    }

    void *dl_android = dlopen("libdl_android.so", RTLD_LOCAL | RTLD_LAZY);
    if (dl_android == NULL) {
        /* A plain dlopen, so this dlerror() is the first and only reader. */
        const char *detail = dlerror();
        eclipseexec_set_error("cannot load libdl_android.so: %s",
                              detail ? detail : "unknown error");
        return false;
    }

    void *get_exported_namespace = dlsym(dl_android, "android_get_exported_namespace");
    if (get_exported_namespace == NULL) {
        eclipseexec_set_error("libdl_android.so does not export android_get_exported_namespace");
        return false;
    }

    typedef void (*arm_fn)(void *, const char *, void *, void *);
    arm_fn arm = (arm_fn)dlsym(hook, "eclipse_hook_arm");
    if (arm == NULL) {
        eclipseexec_set_error("%s does not export eclipse_hook_arm; was it rebuilt?",
                              ECLIPSE_TURNIP_HOOK);
        return false;
    }

    /* The real implementations, resolved here in the application namespace
     * where the interposer is not loaded. Passing them across is what stops
     * the hook from recursing into itself. The soname goes with them so the
     * hook can take its own reference to the driver for each ICD request. */
    arm(driver, ECLIPSE_TURNIP_DRIVER, (void *)android_dlopen_ext,
        get_exported_namespace);

    const char *tmpdir = getenv("TMPDIR");
    if (tmpdir == NULL || tmpdir[0] == '\0') {
        tmpdir = "/data/local/tmp";
    }

    void *loader = eclipse_namespace_dlopen_unique(tmpdir, ECLIPSE_SYSTEM_VULKAN,
                                                   ECLIPSE_LOADER_ALIAS,
                                                   RTLD_LOCAL | RTLD_NOW);
    if (loader == NULL) {
        /* eclipse_namespace_dlopen_unique() has already recorded why. */
        return false;
    }

    pthread_mutex_lock(&g_lock);
    g_turnip_ready = true;
    pthread_mutex_unlock(&g_lock);

    ECLIPSE_LOGI("Turnip wired up: driver %p, system loader %p as %s",
                 driver, loader, ECLIPSE_LOADER_ALIAS);
    return true;
}

/* One caller at a time; the rest line up here rather than loading twice. */
static bool load_turnip(void)
{
    pthread_mutex_lock(&g_load_lock);
    bool ok = load_turnip_locked();
    pthread_mutex_unlock(&g_load_lock);
    return ok;
}

#endif /* ECLIPSE_HAVE_TURNIP */

void eclipseexec_preload_vulkan(void)
{
#ifdef ECLIPSE_HAVE_TURNIP
    pthread_mutex_lock(&g_lock);
    bool wanted = g_turnip_selected;
    pthread_mutex_unlock(&g_lock);

    if (!wanted) {
        ECLIPSE_LOGD("nothing to preload: Turnip was not selected");
        return;
    }
    if (eclipseexec_device_api_level() < ECLIPSE_MIN_TURPIP_API) {
        ECLIPSE_LOGW("Turnip skipped: the loader needs Android %d, this is %d",
                     ECLIPSE_MIN_TURPIP_API, eclipseexec_device_api_level());
        return;
    }
    if (!load_turnip()) {
        ECLIPSE_LOGE("Turnip preload failed: %s", eclipseexec_last_error());
    }
#else
    ECLIPSE_LOGD("built without Turnip support for this ABI");
#endif
}

void *eclipseexec_acq_vulkan_handle(void)
{
    void *handle = NULL;

#ifdef ECLIPSE_HAVE_TURNIP
    pthread_mutex_lock(&g_lock);
    bool turnip = g_turnip_selected;
    pthread_mutex_unlock(&g_lock);

    if (turnip && eclipseexec_device_api_level() >= ECLIPSE_MIN_TURPIP_API && load_turnip()) {
        /* A fresh reference every time, so a library that dlcloses the loader
         * cannot take the driver out from under the rest of the process. */
        handle = eclipse_namespace_dlopen(ECLIPSE_LOADER_ALIAS, RTLD_LOCAL | RTLD_NOW);
        if (handle == NULL) {
            /* Not dlerror(): the namespace code already consumed it. Its
             * message is the one worth logging, and this is only a warning —
            * the caller falls back to the system loader below. */
            ECLIPSE_LOGW("reference to %s failed: %s", ECLIPSE_LOADER_ALIAS,
                         eclipseexec_last_error());
        }
    }
#endif

    if (handle == NULL) {
        handle = dlopen(ECLIPSE_SYSTEM_VULKAN, RTLD_LOCAL | RTLD_NOW);
        if (handle == NULL) {
            const char *detail = dlerror();
            eclipseexec_set_error("cannot load %s: %s", ECLIPSE_SYSTEM_VULKAN,
                                  detail ? detail : "unknown error");
            return NULL;
        }
        ECLIPSE_LOGI("system Vulkan loader at %p", handle);
    }

    eclipseexec_clear_error();
    return handle;
}
