/*
 * eclipseexec — minimal ELF reading.
 * Copyright (c) 2026 Shadow. SPDX-License-Identifier: MIT
 *
 * Two jobs, both done by hand because neither is exposed anywhere useful:
 *
 *  1. Resolve a symbol in a library that is already mapped, by reading its file
 *     and adding the load bias. Used to reach __loader_dlopen inside the linker.
 *  2. Copy a library and rewrite its DT_SONAME, so the same file can be loaded
 *     under a second name.
 *
 * Everything uses ElfW() from <link.h>, which picks 32- or 64-bit structures
 * for the architecture being compiled. There is no elf_defs.h to keep in sync.
 */
#ifndef ECLIPSE_ELF_H
#define ECLIPSE_ELF_H

#include <stddef.h>
#include <stdint.h>

/**
 * Resolve `symbol` in the ELF file at `path`, assuming the file is mapped at
 * `load_base` (as reported by /proc/self/maps).
 *
 * @return the runtime address, or NULL if the file cannot be read or has no
 *         such symbol. NULL is ambiguous with a legitimately zero-valued
 *         symbol, which never occurs for the symbols this library looks up.
 */
void *eclipse_elf_lookup_symbol(const char *path, uintptr_t load_base, const char *symbol);

/**
 * Copy the ELF at `src_path` into a fresh memory file with DT_SONAME replaced
 * by `patched_soname`.
 *
 * @param patched_soname must be the same length as the existing DT_SONAME.
 * @param fallback_dir   directory for an ordinary file when memfd_create is
 *                       unavailable; may be NULL to fail instead.
 * @return a read-only descriptor positioned at 0, or -1 with errno set.
 */
int eclipse_elf_clone_with_soname(const char *src_path, const char *patched_soname,
                                  const char *fallback_dir);

#endif /* ECLIPSE_ELF_H */
