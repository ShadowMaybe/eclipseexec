/*
 * eclipseexec — arm64 fallback route to the loader's dlopen.
 * Copyright (c) 2026 Shadow. SPDX-License-Identifier: MIT
 *
 * The main route (eclipse_namespace.c, route 2) reads __loader_dlopen out of
 * the linker's ELF file. This one exists for devices where that is not
 * possible — a /proc/self/maps that has been locked down, or a linker without
 * a usable symbol table — and works from libdl.so instead.
 *
 * libdl's dlopen() is a thin wrapper around the real implementation, which
 * lives in the linker. The wrapper ends in a PC-relative branch; finding that
 * branch finds the real dlopen. Only arm64: the instruction encoding below is
 * arm64's, not arm32's.
 */
#ifndef ECLIPSE_ARM64_H
#define ECLIPSE_ARM64_H

/**
 * Return the address of the loader's dlopen, or NULL if it could not be found.
 * Only defined when ECLIPSE_HAVE_TURNIP (arm64-v8a) is being built.
 */
void *eclipse_arm64_loader_dlopen(void);

#endif /* ECLIPSE_ARM64_H */
