/*
 * eclipseexec — private linker namespace, the driver bypass.
 * Copyright (c) 2026 Shadow. SPDX-License-Identifier: MIT
 *
 * Android's dlopen() refuses to load a driver from outside the application's
 * namespace, which is exactly where a launcher keeps its Mesa, Turnip or gl4es
 * builds. The workaround is to ask bionic for a namespace of our own and hand
 * it to android_dlopen_ext().
 *
 * android_create_namespace() and android_link_namespaces() are exported by
 * libdl_android.so but not declared in the NDK, and libdl_android.so is not
 * reachable by a plain dlopen() on every release. resolve_api() documents the
 * routes this file takes to reach them, in order.
 *
 * Everything here is isolated behind eclipse_namespace.h so it can be swapped
 * wholesale.
 */

#define ECLIPSE_LOG_TAG "EclipseExec/ns"
#include "eclipse_namespace.h"
#include "eclipse_elf.h"
#include "eclipse_log.h"
#include "eclipse_maps.h"
#include "eclipseexec.h"

#include <android/dlext.h>
#include <dlfcn.h>
#include <errno.h>
#include <linux/limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__arm64__)
#include "eclipse_arm64.h"
#endif

#if defined(__LP64__)
#define ECLIPSE_SYSTEM_LIB "/system/lib64"
#else
#define ECLIPSE_SYSTEM_LIB "/system/lib"
#endif

/*
 * bionic's android_namespace_type_t is not in the NDK. These two bits have not
 * moved since the type was introduced: ISOLATED confines the namespace to its
 * permitted paths, SHARED lets it see libraries already loaded elsewhere. The
 * combination is what a driver namespace needs.
 */
#define ECLIPSE_NS_ISOLATED 1u
#define ECLIPSE_NS_SHARED   2u
/* android_create_namespace() and the ISOLATED/SHARED type bits arrive with
 * linker namespaces in Android 7.0. */
#define ECLIPSE_MIN_NAMESPACE_API 24

/* ---- the namespace API --------------------------------------------------- */

/*
 * These two are bionic's own declarations, from libdl/libdl_android.cpp where
 * the public wrappers are defined. The parameter list is fixed by the ABI:
 * six arguments, in this order, with these types — so every correct
 * declaration of android_create_namespace() is this one, ours included.
 *
 * (bionic's internal __loader_android_create_namespace() takes a seventh,
 * caller_addr, and the public wrapper fills it in with
 * __builtin_return_address(0). dlsym() finds the six-argument wrapper, which
 * is what this type describes.)
 */
typedef struct android_namespace_t *(*create_namespace_fn)(
        const char *name, const char *ld_library_path, const char *default_library_path,
        uint64_t type, const char *permitted_when_isolated_path,
        struct android_namespace_t *parent);

typedef bool (*link_namespaces_fn)(struct android_namespace_t *from,
                                   struct android_namespace_t *to,
                                   const char *shared_libs_sonames);

typedef void *(*loader_dlopen_fn)(const char *name, int flags, const void *caller_addr);

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static create_namespace_fn g_create_namespace;
static link_namespaces_fn g_link_namespaces;
static void *g_dl_android; /* owns the reference the two pointers live in */

static struct android_namespace_t *g_namespace;
static char g_search_path[PATH_MAX];

/*
 * Take ownership of `handle`. On failure it is closed again; on success it is
 * kept open for the life of the process, because closing it would leave the
 * two function pointers dangling.
 */
static bool adopt_api(void *handle, const char *route)
{
    create_namespace_fn create = (create_namespace_fn)dlsym(handle, "android_create_namespace");
    link_namespaces_fn link = (link_namespaces_fn)dlsym(handle, "android_link_namespaces");

    if (create == NULL || link == NULL) {
        ECLIPSE_LOGW("%s: no android_create_namespace/android_link_namespaces there", route);
        dlclose(handle);
        return false;
    }

    g_create_namespace = create;
    g_link_namespaces = link;
    g_dl_android = handle;
    ECLIPSE_LOGI("namespace API reached through %s", route);
    return true;
}

/* Route 1 — libdl_android.so may simply be loadable on this release. */
static bool route_plain_dlopen(void)
{
    dlerror();
    void *handle = dlopen("libdl_android.so", RTLD_NOW | RTLD_LOCAL);
    if (handle == NULL) {
        ECLIPSE_LOGD("plain dlopen(libdl_android.so): %s", dlerror());
        return false;
    }
    return adopt_api(handle, "a plain dlopen");
}

/*
 * Route 2 — find the dynamic loader in our own address space, read
 * __loader_dlopen straight out of its ELF file, and let it open
 * libdl_android.so for us. Works on every ABI; needs a readable /proc/self/maps
 * and a linker with its symbol table intact.
 */
static bool route_linker_symbol(void)
{
    char path[PATH_MAX];
    path[0] = '\0';
    uintptr_t base = 0;

#if defined(__LP64__)
    static const char *needle = "linker64";
#else
    static const char *needle = "/linker";
#endif

    if (!eclipse_maps_find(needle, path, sizeof(path), &base)) {
        ECLIPSE_LOGD("no mapping matching \"%s\" in /proc/self/maps", needle);
        return false;
    }

    loader_dlopen_fn loader_dlopen =
            (loader_dlopen_fn)eclipse_elf_lookup_symbol(path, base, "__loader_dlopen");
    if (loader_dlopen == NULL) {
        return false;
    }

    void *handle = loader_dlopen("libdl_android.so", RTLD_NOW, (const void *)&dlopen);
    if (handle == NULL) {
        const char *detail = dlerror();
        ECLIPSE_LOGW("__loader_dlopen(libdl_android.so): %s", detail ? detail : "unknown error");
        return false;
    }
    return adopt_api(handle, "__loader_dlopen in the linker");
}

#if defined(__arm64__)
/* Route 3 — arm64 only: walk the branch out of dlopen() in libdl.so. The last
 * resort when maps or the linker's symbol table are not available. */
static bool route_arm64_branch(void)
{
    loader_dlopen_fn loader_dlopen = (loader_dlopen_fn)eclipse_arm64_loader_dlopen();
    if (loader_dlopen == NULL) {
        return false;
    }

    void *handle = loader_dlopen("libdl_android.so", RTLD_NOW, (const void *)&dlopen);
    if (handle == NULL) {
        const char *detail = dlerror();
        ECLIPSE_LOGW("__loader_dlopen via the libdl branch: %s",
                     detail ? detail : "unknown error");
        return false;
    }
    return adopt_api(handle, "the branch out of dlopen()");
}
#endif

static bool resolve_api(void)
{
    if (g_create_namespace != NULL && g_link_namespaces != NULL) {
        return true;
    }

    /*
     * Linker namespaces arrived in Android 7.0. Below that there is nothing
     * to reach for, and saying so plainly beats three routes that all fail
     * followed by a message about libdl_android.so. 0 means "unknown", in
     * which case we try anyway and let the routes report.
     */
    int api = eclipseexec_device_api_level();
    if (api != 0 && api < ECLIPSE_MIN_NAMESPACE_API) {
        eclipseexec_set_error("the driver namespace needs API %d, this device is API %d",
                              ECLIPSE_MIN_NAMESPACE_API, api);
        return false;
    }

    if (route_plain_dlopen()) {
        return true;
    }
    if (route_linker_symbol()) {
        return true;
    }
#if defined(__arm64__)
    if (route_arm64_branch()) {
        return true;
    }
#endif

    eclipseexec_set_error("cannot reach android_create_namespace: every route to "
                          "libdl_android.so failed (see logcat for EclipseExec/ns)");
    return false;
}

/* ---- namespace construction --------------------------------------------- */

/* Called with g_lock held. */
static bool init_locked(const char *search_path)
{
    if (g_namespace != NULL && strcmp(g_search_path, search_path) == 0) {
        /* Already built for exactly this directory. If the launcher ever moves
         * its libraries, a second namespace is created and the first is left
         * to the loader to manage. */
        return true;
    }

    if (!resolve_api()) {
        return false;
    }

    char paths[PATH_MAX];
    int written = snprintf(paths, sizeof(paths), "%s:%s", ECLIPSE_SYSTEM_LIB, search_path);
    if (written < 0 || written >= (int)sizeof(paths)) {
        eclipseexec_set_error("driver search path is too long (%zu bytes)",
                              strlen(search_path));
        return false;
    }

    struct android_namespace_t *ns = g_create_namespace(
            "eclipse-driver", paths, paths,
            ECLIPSE_NS_ISOLATED | ECLIPSE_NS_SHARED,
            "/system/:/system_ext/:/data/:/vendor/:/apex/",
            NULL);
    if (ns == NULL) {
        const char *detail = dlerror();
        eclipseexec_set_error("android_create_namespace(\"%s\"): %s", paths,
                              detail ? detail : "unknown error");
        return false;
    }

    /*
     * Three links back into the default namespace. Without ld-android.so the
     * loader's internal __loader lookups fail on a lot of Android versions and
     * nothing loads at all; without the libnativeloader pair, some vendors'
     * builds deadlock while resolving the Vulkan HAL. The second link is the
     * one that has to keep working after the driver is in.
     */
    static const char *const k_shared[] = {
        "ld-android.so",
        "libnativeloader.so",
        "libnativeloader_lazy.so",
    };
    for (size_t i = 0; i < sizeof(k_shared) / sizeof(k_shared[0]); i++) {
        if (!g_link_namespaces(ns, NULL, k_shared[i])) {
            ECLIPSE_LOGW("link_namespaces(%s) refused; continuing anyway", k_shared[i]);
        }
    }

    /* g_search_path holds the directory alone, so this comparison in
     * init_locked() is a plain string match against what the caller passed. */
    snprintf(g_search_path, sizeof(g_search_path), "%s", search_path);
    g_namespace = ns;
    ECLIPSE_LOGI("namespace \"eclipse-driver\" ready, search path: %s", paths);
    return true;
}

bool eclipse_namespace_init(const char *search_path)
{
    if (search_path == NULL || search_path[0] == '\0') {
        eclipseexec_set_error("driver namespace: native library directory is not set");
        return false;
    }

    pthread_mutex_lock(&g_lock);
    bool ok = init_locked(search_path);
    pthread_mutex_unlock(&g_lock);
    return ok;
}

void eclipse_namespace_reset(void)
{
    pthread_mutex_lock(&g_lock);
    g_namespace = NULL;
    g_search_path[0] = '\0';
    pthread_mutex_unlock(&g_lock);

    ECLIPSE_LOGW("driver namespace dropped; the next init rebuilds it");
}

static struct android_namespace_t *current_namespace(const char *name)
{
    pthread_mutex_lock(&g_lock);
    struct android_namespace_t *ns = g_namespace;
    pthread_mutex_unlock(&g_lock);

    if (ns == NULL) {
        eclipseexec_set_error("cannot load %s: the driver namespace has not been created",
                              name != NULL ? name : "(null)");
    }
    return ns;
}

/* ---- loading ------------------------------------------------------------- */

void *eclipse_namespace_dlopen(const char *name, int flags)
{
    if (name == NULL) {
        errno = EINVAL;
        return NULL;
    }

    struct android_namespace_t *ns = current_namespace(name);
    if (ns == NULL) {
        return NULL;
    }

    android_dlextinfo info;
    memset(&info, 0, sizeof(info));
    info.flags = ANDROID_DLEXT_USE_NAMESPACE;
    info.library_namespace = ns;

    void *handle = android_dlopen_ext(name, flags, &info);
    if (handle == NULL) {
        const char *detail = dlerror();
        eclipseexec_set_error("cannot load %s in the driver namespace: %s", name,
                              detail ? detail : "unknown error");
        return NULL;
    }

    return handle;
}

void *eclipse_namespace_dlopen_unique(const char *tmpdir, const char *name,
                                      const char *patched_soname, int flags)
{
    if (name == NULL || patched_soname == NULL) {
        errno = EINVAL;
        return NULL;
    }

    struct android_namespace_t *ns = current_namespace(name);
    if (ns == NULL) {
        return NULL;
    }

    char source[PATH_MAX];
    if (name[0] == '/') {
        int n = snprintf(source, sizeof(source), "%s", name);
        if (n < 0 || n >= (int)sizeof(source)) {
            eclipseexec_set_error("driver path too long: %s", name);
            return NULL;
        }
    } else {
        int n = snprintf(source, sizeof(source), "%s/%s", ECLIPSE_SYSTEM_LIB, name);
        if (n < 0 || n >= (int)sizeof(source)) {
            eclipseexec_set_error("driver path too long: %s/%s", ECLIPSE_SYSTEM_LIB, name);
            return NULL;
        }
    }

    int fd = eclipse_elf_clone_with_soname(source, patched_soname, tmpdir);
    if (fd < 0) {
        eclipseexec_set_error("cannot prepare %s to load as %s: %s", source,
                              patched_soname, strerror(errno));
        return NULL;
    }

    android_dlextinfo info;
    memset(&info, 0, sizeof(info));
    info.flags = ANDROID_DLEXT_USE_NAMESPACE | ANDROID_DLEXT_USE_LIBRARY_FD;
    info.library_fd = fd;
    info.library_namespace = ns;

    void *handle = android_dlopen_ext(patched_soname, flags, &info);
    if (handle == NULL) {
        const char *detail = dlerror();
        eclipseexec_set_error("cannot load %s from its patched copy: %s", patched_soname,
                              detail ? detail : "unknown error");
        close(fd);
        return NULL;
    }

    /*
     * Closed on purpose now. bionic takes library_fd with
     * task->set_fd(fd, false) — it does not own it and never closes it — and
     * by the time android_dlopen_ext() returns the mapping is complete, so
     * nothing is still reading through the descriptor. Leaving it open would
     * leak one descriptor per attempt for no benefit.
     */
    ECLIPSE_LOGI("%s loaded from a patched copy (fd %d)", patched_soname, fd);
    close(fd);
    return handle;
}
