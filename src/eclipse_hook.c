/*
 * eclipseexec — the Vulkan loader interposer.
 * Copyright (c) 2026 Shadow. SPDX-License-Identifier: MIT
 *
 * This file is libeclipsehook.so, loaded into the driver namespace before the
 * Vulkan loader is. Once it is there, the loader's own references to three
 * private bionic interfaces resolve here instead of wherever bionic put them,
 * which is how a driver loaded from the launcher's own directory gets to
 * answer for the hardware ICD.
 *
 * The real implementations are handed over by eclipseexec at arm time. It
 * resolves them in the application namespace, where this library is not
 * loaded, so they cannot point back at us and recurse.
 */

#define ECLIPSE_LOG_TAG "EclipseExec/hook"
#include "eclipse_log.h"

#include <android/dlext.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef void *(*real_dlopen_ext_fn)(const char *filename, int flags,
                                    const android_dlextinfo *extinfo);
typedef void *(*real_get_namespace_fn)(const char *name);

/*
 * No declaration of android_dlopen_ext is repeated here: <android/dlext.h> is
 * included above and already declares it, which is what lets eclipse_hook_arm
 * take its address further down. The definition carries the visibility it
 * needs; a second, hand-written copy of the prototype would only be something
 * to keep in step with the header.
 */

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static real_dlopen_ext_fn g_real_dlopen_ext;
static real_get_namespace_fn g_real_get_exported_namespace;
static void *g_ready_handle;
static const char *g_driver_name; /* soname of g_ready_handle, for fresh references */

/* Read the three armed values as one consistent snapshot. */
static void snapshot(real_dlopen_ext_fn *real_fn, real_get_namespace_fn *get_ns, void **ready)
{
    pthread_mutex_lock(&g_lock);
    *real_fn = g_real_dlopen_ext;
    *get_ns = g_real_get_exported_namespace;
    *ready = g_ready_handle;
    pthread_mutex_unlock(&g_lock);
}

/*
 * The loader asks for the hardware ICD by its HAL name: "vulkan.<ro>.<name>",
 * as a bare name or as a path under /vendor/lib64/hw.
 *
 * The test is on the file name, not on the whole string, because the whole
 * string contains "vulkan." in "libvulkan.so" as well — and answering *that*
 * with the driver would hand the loader the wrong library where it asked for
 * itself.
 */
static bool wants_vulkan_icd(const char *filename)
{
    if (filename == NULL) {
        return false;
    }
    const char *base = strrchr(filename, '/');
    base = (base != NULL) ? base + 1 : filename;
    return strncmp(base, "vulkan.", strlen("vulkan.")) == 0;
}

/*
 * A reference of our own to the driver, taken in the namespace this library is
 * loaded in — which is the namespace the driver is loaded in, so it is already
 * there and this only bumps the count.
 *
 * The armed handle cannot be returned instead. It is the single reference the
 * loader in eclipseexec is holding and will never release, so a caller that
 * does the ordinary thing and dlclose()s what it was given would take the
 * driver out from under the renderer on the first unload. Every ICD request
 * gets its own reference, which is what dlopen() would have handed back.
 */
static void *driver_reference(int flags)
{
    pthread_mutex_lock(&g_lock);
    const char *name = g_driver_name;
    void *armed = g_ready_handle;
    pthread_mutex_unlock(&g_lock);

    if (name == NULL) {
        return NULL; /* not armed; the caller never reaches here */
    }

    void *handle = dlopen(name, flags);
    if (handle == NULL) {
        ECLIPSE_LOGE("reference to %s failed: %s (armed handle was %p)",
                     name, dlerror(), armed);
        return NULL;
    }
    return handle;
}

/*
 * Handed across from eclipseexec: the driver we loaded ourselves, plus the two
 * bionic entry points it must not be allowed to lose. Called exactly once,
 * before the loader is opened.
 */
__attribute__((visibility("default"), used))
void eclipse_hook_arm(void *driver_handle, const char *driver_name,
                      void *android_dlopen_ext_ptr,
                      void *android_get_exported_namespace_ptr)
{
    /* If this ever resolves to our own android_dlopen_ext instead of libdl's,
     * every interposed call below would recurse forever. Refuse to arm. */
    if (android_dlopen_ext_ptr == (void *)&android_dlopen_ext) {
        ECLIPSE_LOGE("android_dlopen_ext resolved to this library, not to libdl; "
                     "the interposer would recurse. Not arming.");
        return;
    }

    pthread_mutex_lock(&g_lock);
    g_ready_handle = driver_handle;
    g_driver_name = driver_name;
    g_real_dlopen_ext = (real_dlopen_ext_fn)android_dlopen_ext_ptr;
    g_real_get_exported_namespace = (real_get_namespace_fn)android_get_exported_namespace_ptr;
    pthread_mutex_unlock(&g_lock);

    ECLIPSE_LOGI("hook armed: driver %p as %s", driver_handle,
                 driver_name ? driver_name : "(unnamed)");
}

/*
 * The interposed loader entry. Anything naming a Vulkan ICD comes back as the
 * driver already in memory; everything else is passed through untouched.
 */
__attribute__((visibility("default"), used))
void *android_dlopen_ext(const char *filename, int flags, const android_dlextinfo *extinfo)
{
    real_dlopen_ext_fn real_fn = NULL;
    real_get_namespace_fn get_ns = NULL;
    void *ready = NULL;
    snapshot(&real_fn, &get_ns, &ready);

    if (ready != NULL && wants_vulkan_icd(filename)) {
        return driver_reference(flags);
    }

    if (real_fn != NULL) {
        return real_fn(filename, flags, extinfo);
    }

    /* Loaded but never armed: fall back to a plain load in this namespace,
     * which is the closest this library can get to the original request. */
    ECLIPSE_LOGW("android_dlopen_ext(%s) before arming", filename ? filename : "(null)");
    return dlopen(filename, flags);
}

/*
 * Vendor libraries are loaded through this, not android_dlopen_ext. The ICD
 * case is answered the same way; the rest is routed to the exported vendor
 * namespace, which is where bionic would have sent it.
 */
__attribute__((visibility("default"), used))
void *android_load_sphal_library(const char *filename, int flags)
{
    real_dlopen_ext_fn real_fn = NULL;
    real_get_namespace_fn get_ns = NULL;
    void *ready = NULL;
    snapshot(&real_fn, &get_ns, &ready);

    if (ready != NULL && wants_vulkan_icd(filename)) {
        return driver_reference(flags);
    }

    if (get_ns != NULL) {
        static const char *const k_namespaces[] = {"sphal", "vendor", "default"};
        for (size_t i = 0; i < sizeof(k_namespaces) / sizeof(k_namespaces[0]); i++) {
            struct android_namespace_t *ns = get_ns(k_namespaces[i]);
            if (ns == NULL) {
                continue;
            }
            if (real_fn != NULL) {
                android_dlextinfo info;
                memset(&info, 0, sizeof(info));
                info.flags = ANDROID_DLEXT_USE_NAMESPACE;
                info.library_namespace = ns;
                return real_fn(filename, flags, &info);
            }
            break;
        }
    }

    if (real_fn != NULL) {
        return real_fn(filename, flags, NULL);
    }

    ECLIPSE_LOGW("android_load_sphal_library(%s) before arming", filename ? filename : "(null)");
    return dlopen(filename, flags);
}

/*
 * Present so that libraries referencing it resolve on releases where bionic
 * does not export it. Returning "no tracing enabled" is the honest answer for
 * a game process, and it is what the reference implementations of this trick
 * have always returned.
 */
__attribute__((visibility("default"), used))
uint64_t atrace_get_enabled_tags(void)
{
    return 0;
}
