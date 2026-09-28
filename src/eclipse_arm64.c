/*
 * eclipseexec — arm64 fallback route to the loader's dlopen.
 * Copyright (c) 2026 Shadow. SPDX-License-Identifier: MIT
 *
 * The main route (eclipse_namespace.c, route 2) reads __loader_dlopen out of
 * the linker's ELF file. This one exists for devices where that is not
 * possible — a locked-down /proc/self/maps, a linker without a usable symbol
 * table — and works from libdl.so instead.
 *
 * libdl's dlopen() is a wrapper, not an algorithm: a few instructions that end
 * in a PC-relative branch to the real thing, which lives in the linker. Find
 * that branch and you have the address. arm64 only: the encoding decoded below
 * is arm64's, not arm32's.
 */

#define ECLIPSE_LOG_TAG "EclipseExec/arm64"
#include "eclipse_arm64.h"
#include "eclipse_elf.h"
#include "eclipse_log.h"
#include "eclipse_maps.h"

#include <dlfcn.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/*
 * AArch64 unconditional branches: the top six bits are 000101 for B and
 * 100101 for BL, and the low 26 bits hold a signed offset in instructions.
 * Conditional branches (B.cond, CBZ, TBZ) have different top bits and are
 * correctly ignored by this mask.
 */
#define ECLIPSE_INSN_MASK 0xFC000000u
#define ECLIPSE_INSN_B    0x14000000u
#define ECLIPSE_INSN_BL   0x94000000u

/* dlopen() is a wrapper; if the branch is not in the first few words, this is
 * not the wrapper we were expecting and we should stop reading. */
#define ECLIPSE_MAX_SCAN_WORDS 64

static bool is_unconditional_branch(uint32_t insn)
{
    uint32_t top = insn & ECLIPSE_INSN_MASK;
    return top == ECLIPSE_INSN_B || top == ECLIPSE_INSN_BL;
}

static intptr_t branch_displacement(uint32_t insn)
{
    int32_t words = (int32_t)(insn & 0x03FFFFFFu);
    if (words & 0x02000000) {
        words -= 0x04000000; /* sign-extend: the offset may point backwards */
    }
    return (intptr_t)words * 4;
}

/* Address of the first unconditional branch in the window, or NULL. */
static uintptr_t first_branch_target(const uint32_t *code, size_t words)
{
    for (size_t i = 0; i < words; i++) {
        if (is_unconditional_branch(code[i])) {
            return (uintptr_t)&code[i] + branch_displacement(code[i]);
        }
    }
    return 0;
}

void *eclipse_arm64_loader_dlopen(void)
{
    long pagesize = sysconf(_SC_PAGESIZE);
    if (pagesize <= 0) {
        pagesize = 4096;
    }

    uintptr_t code = (uintptr_t)(void *)&dlopen;

    /* Some vendors mark libdl's text execute-only. Making our own process's
     * page readable before walking it is the point of the mprotect. */
    uintptr_t page = code & ~((uintptr_t)pagesize - 1);
    if (mprotect((void *)page, (size_t)pagesize, PROT_READ | PROT_EXEC) != 0) {
        ECLIPSE_LOGW("mprotect(%p): %s", (void *)page, strerror(errno));
        return NULL;
    }

    /* Never read past the end of the page we just made readable. */
    size_t words = (page + (uintptr_t)pagesize - code) / sizeof(uint32_t);
    if (words > ECLIPSE_MAX_SCAN_WORDS) {
        words = ECLIPSE_MAX_SCAN_WORDS;
    }

    uintptr_t target = first_branch_target((const uint32_t *)code, words);
    if (target == 0) {
        ECLIPSE_LOGW("no unconditional branch in the first %zu words of dlopen", words);
        return NULL;
    }

    /*
     * The branch has to land on __loader_dlopen — that is the whole claim
     * being made, and "somewhere in the linker" is a weaker one. The first
     * unconditional branch in the window belongs to whatever function it
     * belongs to, and calling that with dlopen's three arguments would pass a
     * garbage extinfo to a four-argument entry. Resolve the real symbol out of
     * the file the branch lands in and insist the two agree.
     */
    char owner[512] = "";
    if (!eclipse_maps_path_at(target, owner, sizeof(owner)) || strstr(owner, "linker") == NULL) {
        ECLIPSE_LOGW("branch out of dlopen() lands at %p in \"%s\", not in the linker",
                     (void *)target, owner);
        return NULL;
    }

    uintptr_t load_base = 0;
    if (!eclipse_maps_find(owner, NULL, 0, &load_base)) {
        ECLIPSE_LOGW("cannot locate the load base of %s", owner);
        return NULL;
    }

    void *expected = eclipse_elf_lookup_symbol(owner, load_base, "__loader_dlopen");
    if (expected == NULL) {
        ECLIPSE_LOGW("%s does not expose __loader_dlopen; refusing an unverified branch",
                     owner);
        return NULL;
    }
    if (expected != (void *)target) {
        ECLIPSE_LOGW("branch out of dlopen() lands at %p, but __loader_dlopen is at %p",
                     (void *)target, expected);
        return NULL;
    }

    uintptr_t target_page = target & ~((uintptr_t)pagesize - 1);
    if (mprotect((void *)target_page, (size_t)pagesize, PROT_READ | PROT_EXEC) != 0) {
        ECLIPSE_LOGW("mprotect of the branch target: %s", strerror(errno));
        return NULL;
    }

    ECLIPSE_LOGI("branch out of dlopen() lands on __loader_dlopen at %p in %s",
                 (void *)target, owner);
    return (void *)target;
}
