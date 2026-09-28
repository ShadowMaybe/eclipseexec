/*
 * eclipseexec — minimal ELF reading.
 * Copyright (c) 2026 Shadow. SPDX-License-Identifier: MIT
 *
 * Two jobs, both done by hand because neither is exposed anywhere useful:
 *
 *  1. Resolve a symbol in a library that is already mapped, by reading its file
 *     and adding the load bias. Used to reach __loader_dlopen inside the linker.
 *  2. Copy a library and rewrite its DT_SONAME, so one file can be loaded under
 *     a second identity when the loader has to see two "different" objects.
 *
 * Everything uses ElfW() from <link.h>, which picks 32- or 64-bit structures
 * for the architecture being compiled — there is no elf_defs.h to keep in sync.
 */

#define ECLIPSE_LOG_TAG "EclipseExec/elf"
#include "eclipse_elf.h"
#include "eclipse_log.h"

#include <errno.h>
#include <fcntl.h>
#include <link.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef ElfW
#if defined(__LP64__)
#define ElfW(type) Elf64_##type
#else
#define ElfW(type) Elf32_##type
#endif
#endif

/* Refuse anything absurd rather than trusting the file to be well formed. */
#define ECLIPSE_MAX_ELF_SIZE (256 * 1024 * 1024)

static bool bounds_ok(size_t file_size, size_t offset, size_t length)
{
    if (offset > file_size) {
        return false;
    }
    return length <= file_size - offset;
}

/*
 * The load bias: the address the file is mapped at, minus the virtual address
 * of its first PT_LOAD segment. Usually the second term is zero, but assuming
 * that would silently return wrong pointers for anything unusual.
 */
static uintptr_t elf_load_bias(const char *image, size_t size, uintptr_t load_base)
{
    const ElfW(Ehdr) *ehdr = (const ElfW(Ehdr) *)image;
    if (!bounds_ok(size, ehdr->e_phoff, (size_t)ehdr->e_phnum * sizeof(ElfW(Phdr)))) {
        return load_base;
    }

    const ElfW(Phdr) *phdr = (const ElfW(Phdr) *)(image + ehdr->e_phoff);
    uintptr_t min_vaddr = UINTPTR_MAX;
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_LOAD && phdr[i].p_vaddr < min_vaddr) {
            min_vaddr = (uintptr_t)phdr[i].p_vaddr;
        }
    }
    if (min_vaddr == UINTPTR_MAX) {
        return load_base;
    }
    return load_base - min_vaddr;
}

static void *lookup_in_image(const char *image, size_t size, uintptr_t load_base, const char *symbol)
{
    const ElfW(Ehdr) *ehdr = (const ElfW(Ehdr) *)image;
    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0) {
        ECLIPSE_LOGE("not an ELF file");
        return NULL;
    }
    if (!bounds_ok(size, ehdr->e_shoff, (size_t)ehdr->e_shnum * sizeof(ElfW(Shdr)))) {
        ECLIPSE_LOGE("section header table out of bounds");
        return NULL;
    }

    const ElfW(Shdr) *shdr = (const ElfW(Shdr) *)(image + ehdr->e_shoff);
    const uintptr_t bias = elf_load_bias(image, size, load_base);
    void *result = NULL;

    for (int i = 0; i < ehdr->e_shnum && result == NULL; i++) {
        if (shdr[i].sh_type != SHT_DYNSYM) {
            continue;
        }
        if (shdr[i].sh_link >= ehdr->e_shnum) {
            break;
        }
        const ElfW(Shdr) *strtab = &shdr[shdr[i].sh_link];
        if (!bounds_ok(size, shdr[i].sh_offset, shdr[i].sh_size) ||
            !bounds_ok(size, strtab->sh_offset, strtab->sh_size) ||
            shdr[i].sh_entsize == 0) {
            break;
        }

        const ElfW(Sym) *syms = (const ElfW(Sym) *)(image + shdr[i].sh_offset);
        const char *strings = image + strtab->sh_offset;
        size_t count = shdr[i].sh_size / shdr[i].sh_entsize;

        for (size_t s = 0; s < count; s++) {
            size_t offset = (size_t)syms[s].st_name;
            /* Being inside the table is not enough: only a NUL *within* it
             * proves the string stops before the end of the mapping. Without
             * that check strcmp() walks past the mmap on a malformed file. */
            if (offset >= strtab->sh_size ||
                memchr(strings + offset, '\0', strtab->sh_size - offset) == NULL) {
                continue;
            }
            if (strcmp(strings + offset, symbol) != 0) {
                continue;
            }
            if (syms[s].st_shndx == SHN_UNDEF) {
                break; /* a reference to the symbol, not a definition */
            }
            if (syms[s].st_value == 0) {
                break;
            }
            result = (void *)(bias + (uintptr_t)syms[s].st_value);
            break;
        }
    }

    return result;
}

void *eclipse_elf_lookup_symbol(const char *path, uintptr_t load_base, const char *symbol)
{
    if (path == NULL || symbol == NULL) {
        return NULL;
    }

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        ECLIPSE_LOGE("open(%s): %s", path, strerror(errno));
        return NULL;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < (off_t)sizeof(ElfW(Ehdr)) ||
        st.st_size > ECLIPSE_MAX_ELF_SIZE) {
        ECLIPSE_LOGE("%s: bad file size", path);
        close(fd);
        return NULL;
    }

    size_t size = (size_t)st.st_size;
    void *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        ECLIPSE_LOGE("mmap(%s): %s", path, strerror(errno));
        return NULL;
    }

    void *result = lookup_in_image(map, size, load_base, symbol);
    if (result == NULL) {
        ECLIPSE_LOGE("%s: symbol %s not found", path, symbol);
    }

    munmap(map, size);
    return result;
}

/* Read a whole file into a fresh buffer. Returns NULL with errno set. */
static char *slurp(const char *path, size_t *out_size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return NULL;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0 || st.st_size > ECLIPSE_MAX_ELF_SIZE) {
        errno = EFBIG;
        close(fd);
        return NULL;
    }

    size_t size = (size_t)st.st_size;
    char *buffer = malloc(size);
    if (buffer == NULL) {
        close(fd);
        errno = ENOMEM;
        return NULL;
    }

    size_t done = 0;
    while (done < size) {
        ssize_t n = read(fd, buffer + done, size - done);
        if (n <= 0) {
            free(buffer);
            close(fd);
            errno = (n == 0) ? EIO : errno;
            return NULL;
        }
        done += (size_t)n;
    }

    close(fd);
    *out_size = size;
    return buffer;
}

/*
 * Replace DT_SONAME in an in-memory copy of an ELF image.
 * Returns false when the image has no DT_SONAME or the replacement is a
 * different length — the entry is edited in place, so nothing after it may
 * move.
 */
static bool patch_soname(char *image, size_t size, const char *patched_soname)
{
    ElfW(Ehdr) *ehdr = (ElfW(Ehdr) *)image;
    if (!bounds_ok(size, ehdr->e_shoff, (size_t)ehdr->e_shnum * sizeof(ElfW(Shdr)))) {
        ECLIPSE_LOGE("section headers out of bounds");
        return false;
    }

    ElfW(Shdr) *shdr = (ElfW(Shdr) *)(image + ehdr->e_shoff);
    for (int i = 0; i < ehdr->e_shnum; i++) {
        if (shdr[i].sh_type != SHT_DYNAMIC) {
            continue;
        }
        if (shdr[i].sh_link >= ehdr->e_shnum) {
            break;
        }

        ElfW(Shdr) *strtab = &shdr[shdr[i].sh_link];
        if (!bounds_ok(size, shdr[i].sh_offset, shdr[i].sh_size) ||
            !bounds_ok(size, strtab->sh_offset, strtab->sh_size) ||
            shdr[i].sh_entsize == 0) {
            break;
        }

        ElfW(Dyn) *dyn = (ElfW(Dyn) *)(image + shdr[i].sh_offset);
        size_t entries = shdr[i].sh_size / shdr[i].sh_entsize;
        char *strings = image + strtab->sh_offset;

        for (size_t d = 0; d < entries && dyn[d].d_tag != DT_NULL; d++) {
            if (dyn[d].d_tag != DT_SONAME) {
                continue;
            }
            /* The offset is compared as unsigned: on a 32-bit ABI the field
             * comes out of elf.h signed, and a negative offset is out of
             * range by any reading. */
            uint64_t soname_offset = (uint64_t)dyn[d].d_un.d_val;
            if (soname_offset >= (uint64_t)strtab->sh_size) {
                ECLIPSE_LOGE("DT_SONAME offset %llu out of range",
                             (unsigned long long)soname_offset);
                return false;
            }

            char *current = strings + (size_t)soname_offset;
            /* Same rule as the symbol reader: the offset is in range, but the
             * string only exists if its terminator is in range too. */
            size_t span = (size_t)((uint64_t)strtab->sh_size - soname_offset);
            if (memchr(current, '\0', span) == NULL) {
                ECLIPSE_LOGE("DT_SONAME string at offset %llu has no terminator "
                             "inside the string table",
                             (unsigned long long)soname_offset);
                return false;
            }
            size_t current_len = strlen(current);
            size_t wanted_len = strlen(patched_soname);

            if (current_len == wanted_len) {
                memcpy(current, patched_soname, wanted_len);
                return true;
            }
            if (current_len > wanted_len) {
                /* A shorter name is safe: the trailing bytes belong to this
                 * entry alone and no offset in the file points past its NUL. */
                memcpy(current, patched_soname, wanted_len);
                current[wanted_len] = '\0';
                return true;
            }

            ECLIPSE_LOGE("DT_SONAME \"%s\" is shorter than the replacement \"%s\"; "
                         "they must match or the string table would shift",
                         current, patched_soname);
            return false;
        }
    }

    ECLIPSE_LOGE("no DT_SONAME entry found");
    return false;
}

/* A fresh, empty descriptor: memfd first, an unlinked file second. */
static int make_backing_store(const char *fallback_dir)
{
    int fd = (int)syscall(__NR_memfd_create, "eclipseexec", MFD_CLOEXEC);
    if (fd >= 0) {
        return fd;
    }

    if (fallback_dir == NULL) {
        return -1;
    }

    static unsigned counter = 0;
    char path[512];
    snprintf(path, sizeof(path), "%s/eclipseexec-%d-%u.so",
             fallback_dir, (int)getpid(), counter++);

    fd = open(path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        ECLIPSE_LOGE("cannot create %s: %s", path, strerror(errno));
        return -1;
    }
    /* The descriptor keeps the inode alive after unlink, and nothing else has
     * business reading a half-written driver. */
    unlink(path);
    return fd;
}

int eclipse_elf_clone_with_soname(const char *src_path, const char *patched_soname,
                                  const char *fallback_dir)
{
    if (src_path == NULL || patched_soname == NULL) {
        errno = EINVAL;
        return -1;
    }

    size_t size = 0;
    char *image = slurp(src_path, &size);
    if (image == NULL) {
        ECLIPSE_LOGE("cannot read %s: %s", src_path, strerror(errno));
        return -1;
    }

    if (!patch_soname(image, size, patched_soname)) {
        ECLIPSE_LOGE("%s: SONAME patch to \"%s\" failed", src_path, patched_soname);
        free(image);
        errno = EINVAL;
        return -1;
    }

    int fd = make_backing_store(fallback_dir);
    if (fd < 0) {
        ECLIPSE_LOGE("no backing store for the patched copy: %s", strerror(errno));
        free(image);
        return -1;
    }

    size_t done = 0;
    while (done < size) {
        ssize_t n = write(fd, image + done, size - done);
        if (n < 0) {
            ECLIPSE_LOGE("writing the patched copy: %s", strerror(errno));
            close(fd);
            free(image);
            return -1;
        }
        done += (size_t)n;
    }
    free(image);

    if (lseek(fd, 0, SEEK_SET) < 0) {
        ECLIPSE_LOGE("rewinding the patched copy: %s", strerror(errno));
        close(fd);
        return -1;
    }

    /* ftruncate is not strictly needed — the file was never longer — but the
     * ELF loader trusts the descriptor's size, so make it explicit. */
    if (ftruncate(fd, (off_t)size) != 0) {
        ECLIPSE_LOGW("ftruncate: %s", strerror(errno));
    }

    return fd;
}
