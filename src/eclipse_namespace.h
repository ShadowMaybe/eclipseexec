/*
 * eclipseexec — private linker namespace, the driver bypass.
 * Copyright (c) 2026 Shadow. SPDX-License-Identifier: MIT
 *
 * Android's dlopen() refuses to load a driver that sits outside the
 * application's namespace, which is exactly where a launcher keeps its Mesa,
 * Turnip or gl4es builds. The workaround is to ask bionic for a namespace of
 * our own and hand it to android_dlopen_ext().
 *
 * android_create_namespace() and android_link_namespaces() are exported by
 * libdl_android.so but not declared in the NDK, and libdl_android.so itself is
 * not reachable by a plain dlopen() on most releases. eclipse_namespace_init()
 * documents the three routes this file takes to reach them, in order.
 *
 * Everything in here is isolated behind this header so it can be swapped out
 * wholesale.
 */
#ifndef ECLIPSE_NAMESPACE_H
#define ECLIPSE_NAMESPACE_H

#include <stdbool.h>

/**
 * Create the driver namespace, once, with `search_path` appended to the system
 * library path.
 *
 * @param search_path directory holding the drivers, normally the launcher's
 *                    nativeLibraryDir. Must not be NULL.
 * @return true when the namespace exists, whether this call created it or not.
 */
bool eclipse_namespace_init(const char *search_path);

/** Load `name` from the driver namespace. NULL on failure, errno/dlerror set. */
void *eclipse_namespace_dlopen(const char *name, int flags);

/**
 * Load `/system/lib[64]/<name>` from the driver namespace after rewriting its
 * DT_SONAME to `patched_soname`, so the loader will register it under a second
 * identity. Needed when the same file has to be loaded twice — the system Vulkan
 * loader and Turnip's copy of it are different objects as far as the linker is
 * concerned, but they come from one file.
 *
 * `patched_soname` must be exactly as long as `name`; the DT_SONAME entry is
 * edited in place and nothing else in the string table moves.
 *
 * @param tmpdir fallback directory for the patched copy when memfd_create is
 *               unavailable. Ignored on success.
 * @return a handle, or NULL on failure with eclipseexec_last_error() set.
 */
void *eclipse_namespace_dlopen_unique(const char *tmpdir, const char *name,
                                      const char *patched_soname, int flags);

/** Drop the cached namespace so the next init rebuilds it for a new search path. */
void eclipse_namespace_reset(void);

#endif /* ECLIPSE_NAMESPACE_H */
