/*
 * eclipseexec — the fixture the maps/ELF tests locate.
 * Copyright (c) 2026 Shadow. SPDX-License-Identifier: MIT
 *
 * Built as libfixture.so by tests/run_tests.sh. The test dlopens it, finds it
 * again in /proc/self/maps, and resolves this symbol out of the file on disk,
 * which is exactly the trick eclipse_namespace.c uses on the linker.
 */

int eclipse_fixture_marker(void)
{
    return 42;
}
