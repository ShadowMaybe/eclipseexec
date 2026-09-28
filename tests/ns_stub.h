/*
 * eclipseexec — test double interface for the private linker namespace.
 * Copyright (c) 2026 Shadow. SPDX-License-Identifier: MIT
 */

#ifndef ECLIPSE_NS_STUB_H
#define ECLIPSE_NS_STUB_H

#include <stdbool.h>

/* What the code under test asked the (stubbed) namespace to do. */
struct ns_call_log {
    int inits;
    int dlopens;
    int uniques;
    char last_init_path[512];
    char last_dlopen_name[512];
    char last_unique_name[512];
    char last_unique_alias[512];
    bool init_result;
};

extern struct ns_call_log g_ns_log;

/**
 * Clear the log. `init_result` is what eclipse_namespace_init() will return,
 * so a test can make namespace creation itself fail.
 */
void ns_stub_reset(bool init_result);

#endif /* ECLIPSE_NS_STUB_H */
