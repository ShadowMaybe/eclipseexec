/*
 * eclipseexec — host test doubles for the private linker namespace.
 * Copyright (c) 2026 Shadow. SPDX-License-Identifier: MIT
 *
 * The real eclipse_namespace.c needs bionic. The tests only need to know
 * that the callers above it ask for the right thing, in the right order, with
 * the right arguments — so these stand in for it and record what happened.
 */

#include "ns_stub.h"
#include "eclipse_namespace.h"
#include "eclipseexec.h"

#include <stdio.h>
#include <string.h>

struct ns_call_log g_ns_log;

/* A handle that is definitely not NULL and definitely not a real library. */
static char g_fake_handle_storage;
#define FAKE_HANDLE ((void *)&g_fake_handle_storage)

void ns_stub_reset(bool init_result)
{
    memset(&g_ns_log, 0, sizeof(g_ns_log));
    g_ns_log.init_result = init_result;
}

bool eclipse_namespace_init(const char *search_path)
{
    g_ns_log.inits++;
    snprintf(g_ns_log.last_init_path, sizeof(g_ns_log.last_init_path), "%s",
             search_path != NULL ? search_path : "(null)");
    if (!g_ns_log.init_result) {
        eclipseexec_set_error("stub namespace refused to initialise");
        return false;
    }
    return true;
}

void *eclipse_namespace_dlopen(const char *name, int flags)
{
    (void)flags;
    g_ns_log.dlopens++;
    snprintf(g_ns_log.last_dlopen_name, sizeof(g_ns_log.last_dlopen_name), "%s",
             name != NULL ? name : "(null)");

    /* One deliberately absent file, so the failure path can be asserted on. */
    if (strcmp(g_ns_log.last_dlopen_name, "/nonexistent/libGL.so") == 0) {
        eclipseexec_set_error("cannot load %s in the driver namespace: no such file", name);
        return NULL;
    }
    return FAKE_HANDLE;
}

void *eclipse_namespace_dlopen_unique(const char *tmpdir, const char *name,
                                      const char *patched_soname, int flags)
{
    (void)tmpdir;
    (void)flags;
    g_ns_log.uniques++;
    snprintf(g_ns_log.last_unique_name, sizeof(g_ns_log.last_unique_name), "%s",
             name != NULL ? name : "(null)");
    snprintf(g_ns_log.last_unique_alias, sizeof(g_ns_log.last_unique_alias), "%s",
             patched_soname != NULL ? patched_soname : "(null)");
    return FAKE_HANDLE;
}

void eclipse_namespace_reset(void)
{
    g_ns_log.inits = 0;
}
