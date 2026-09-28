/*
 * eclipseexec — host unit tests.
 * Copyright (c) 2026 Shadow. SPDX-License-Identifier: MIT
 *
 * Runs on a plain Linux box or CI runner: no emulator, no device. The parts
 * that need bionic (the namespace, the Turnip path, JNI) are exercised by the
 * Android build instead — see .github/workflows/ci.yml.
 *
 * Build and run with tests/run_tests.sh.
 */

#include "eclipseexec.h"
#include "eclipse_elf.h"
#include "eclipse_maps.h"
#include "ns_stub.h"

#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

#define CHECK(cond, ...) \
    do { \
        if (!(cond)) { \
            failures++; \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__); \
            fputc('\n', stderr); \
        } \
    } while (0)

/* ---- configuration and error reporting --------------------------------- */

static void test_configuration(void)
{
    eclipseexec_clear_error();

    CHECK(!eclipseexec_set_native_dir(NULL),
          "a null native library directory must be rejected");
    CHECK(strstr(eclipseexec_last_error(), "native library directory") != NULL,
          "the message should name what was wrong, got \"%s\"", eclipseexec_last_error());

    CHECK(!eclipseexec_set_native_dir(""),
          "an empty native library directory must be rejected");

    CHECK(eclipseexec_set_native_dir("/data/app/lib"), "a real directory is accepted");
    const char *recorded = eclipseexec_native_dir;
    CHECK(recorded != NULL && strcmp(recorded, "/data/app/lib") == 0,
          "published dir is \"%s\"", recorded ? recorded : "(null)");

    /* Setting the same directory again must not allocate a new copy: the
     * pointer stays stable, which is what other threads rely on. */
    CHECK(eclipseexec_set_native_dir("/data/app/lib"), "same directory again");
    CHECK(eclipseexec_native_dir == recorded,
          "an identical directory keeps the same pointer");

    eclipseexec_set_display_params(1920, 1080, 59.94f);
    CHECK(eclipseexec_renderspec.disp_width == 1920, "width");
    CHECK(eclipseexec_renderspec.disp_height == 1080, "height");
    CHECK(eclipseexec_renderspec.disp_hz > 59.9f && eclipseexec_renderspec.disp_hz < 60.0f,
          "refresh rate %f", (double)eclipseexec_renderspec.disp_hz);

    CHECK(eclipseexec_device_api_level() == 0,
          "off Android the API level is reported as unknown");

    CHECK(eclipseexec_last_error() != NULL, "last_error is never null");
    eclipseexec_clear_error();
    CHECK(eclipseexec_last_error()[0] == '\0', "clear_error empties the buffer");
}

/* ---- the one check that needs a virgin process -------------------------- */

/*
 * This has to run before anything else sets the native library directory:
 * there is no "unset" call, and a launcher should not need one. Split out
 * from test_egl_paths() precisely so the ordering is explicit rather than
 * accidental.
 */
static void test_bypass_before_library_dir(void)
{
    ns_stub_reset(true);
    eclipseexec_clear_error();
    CHECK(eclipseexec_native_dir == NULL, "the native library directory starts unset");

    CHECK(!eclipseexec_prepare_egl("/data/app/lib/libGL.so", true, true, 3),
          "the bypass cannot work before setNativeLibraryDir()");
    CHECK(strstr(eclipseexec_last_error(), "setNativeLibraryDir") != NULL,
          "the message should say what to do first, got \"%s\"", eclipseexec_last_error());
    CHECK(g_ns_log.inits == 0, "no namespace was created for a rejected call");
}

/* ---- EGL preparation ---------------------------------------------------- */

static void test_egl_paths(void)
{
    ns_stub_reset(true);

    eclipseexec_clear_error();
    CHECK(!eclipseexec_prepare_egl(NULL, false, true, 3),
          "prepare_egl with no path must fail");
    CHECK(!eclipseexec_prepare_egl("", false, true, 3),
          "prepare_egl with an empty path must fail");

    /* With the directory set, the whole path goes through. */
    eclipseexec_clear_error();
    CHECK(eclipseexec_prepare_egl("/data/app/lib/libGL.so", true, true, 3),
          "prepare_egl should succeed, said \"%s\"", eclipseexec_last_error());
    CHECK(g_ns_log.inits == 1, "the namespace was created once, got %d", g_ns_log.inits);
    CHECK(strcmp(g_ns_log.last_init_path, "/data/app/lib") == 0,
          "the namespace search path is \"%s\"", g_ns_log.last_init_path);
    CHECK(eclipseexec_renderspec.force_gles_context == 1, "force_gles is published");
    CHECK(eclipseexec_renderspec.override_major_version == 3, "GLES major is published");
    CHECK(eclipseexec_acq_egl_handle() != NULL, "the handle can be acquired again");
    CHECK(g_ns_log.dlopens >= 1, "the driver went through the namespace");
    CHECK(eclipseexec_last_error()[0] == '\0', "a successful call leaves no error");

    /* A driver that is not there must say which one, and keep the namespace's
     * wording rather than swallowing it. */
    eclipseexec_clear_error();
    CHECK(!eclipseexec_prepare_egl("/nonexistent/libGL.so", true, true, 3),
          "a missing driver must fail");
    CHECK(strstr(eclipseexec_last_error(), "/nonexistent/libGL.so") != NULL,
          "the message should name the file, got \"%s\"", eclipseexec_last_error());

    /* The plain dlopen path, no namespace involved. */
    eclipseexec_clear_error();
    CHECK(!eclipseexec_prepare_egl("/nonexistent/libGL.so", false, true, 3),
          "dlopen of a missing file must fail");
    CHECK(strstr(eclipseexec_last_error(), "/nonexistent/libGL.so") != NULL,
          "the message should name the file, got \"%s\"", eclipseexec_last_error());

    /* Namespace creation itself failing. */
    ns_stub_reset(false);
    eclipseexec_clear_error();
    CHECK(!eclipseexec_prepare_egl("/data/app/lib/libGL.so", true, true, 3),
          "a namespace that will not build must fail the preparation");
    CHECK(strstr(eclipseexec_last_error(), "namespace") != NULL,
          "the message should mention the namespace, got \"%s\"", eclipseexec_last_error());
}

/* ---- big-core affinity -------------------------------------------------- */

static void test_affinity(void)
{
    eclipseexec_set_affinity_enabled(false);
    CHECK(!eclipseexec_affinity_enabled(), "affinity reports itself disabled");

    eclipseexec_clear_error();
    eclipseexec_make_bigcore_affine();
    CHECK(eclipseexec_last_error()[0] == '\0',
          "a disabled helper must be silent, got \"%s\"", eclipseexec_last_error());

    eclipseexec_set_affinity_enabled(true);
    CHECK(eclipseexec_affinity_enabled(), "affinity reports itself enabled");

    eclipseexec_clear_error();
    eclipseexec_make_bigcore_affine();
    const char *error = eclipseexec_last_error();
    if (error[0] != '\0') {
        /* It either worked, or it explains itself — the two things that can
         * go wrong are the missing sysfs entries and the syscall itself. */
        CHECK(strstr(error, "cpufreq") != NULL || strstr(error, "sched_setaffinity") != NULL,
              "an affinity failure must name its cause, got \"%s\"", error);
    }

    char snapshot[512];
    snprintf(snapshot, sizeof(snapshot), "%s", eclipseexec_last_error());
    eclipseexec_make_bigcore_affine();
    CHECK(strcmp(snapshot, eclipseexec_last_error()) == 0,
          "the second call on a thread must change nothing");
}

/* ---- maps and ELF ------------------------------------------------------- */

/* Read a descriptor from the start into a fresh buffer. */
static char *read_all(int fd, size_t *out_len)
{
    if (lseek(fd, 0, SEEK_SET) < 0) {
        return NULL;
    }

    size_t capacity = 4096;
    size_t length = 0;
    char *buffer = malloc(capacity);
    if (buffer == NULL) {
        return NULL;
    }

    for (;;) {
        if (length == capacity) {
            capacity *= 2;
            char *grown = realloc(buffer, capacity);
            if (grown == NULL) {
                free(buffer);
                return NULL;
            }
            buffer = grown;
        }

        ssize_t count = read(fd, buffer + length, capacity - length);
        if (count < 0) {
            free(buffer);
            return NULL;
        }
        if (count == 0) {
            break;
        }
        length += (size_t)count;
    }

    *out_len = length;
    return buffer;
}

static void test_maps_and_elf(const char *fixture_path)
{
    void *loaded = dlopen(fixture_path, RTLD_NOW | RTLD_LOCAL);
    /* Read once: dlerror() hands back the message by clearing it, so calling
     * it twice in the same expression loses the text on the first read. */
    const char *why = dlerror();
    CHECK(loaded != NULL, "dlopen(%s): %s", fixture_path, why ? why : "?");
    if (loaded == NULL) {
        return;
    }

    void *expected = dlsym(loaded, "eclipse_fixture_marker");
    CHECK(expected != NULL, "dlsym(eclipse_fixture_marker)");
    if (expected == NULL) {
        return;
    }

    /* Find the library in our own address space. */
    char path[512];
    path[0] = '\0';
    uintptr_t base = 0;
    CHECK(eclipse_maps_find("libfixture.so", path, sizeof(path), &base),
          "libfixture.so should be visible in /proc/self/maps");
    CHECK(base != 0, "the load base should not be zero");
    CHECK(strstr(path, "libfixture.so") != NULL, "maps returned \"%s\"", path);

    /* Resolve the same symbol out of the file, the way the namespace code
     * resolves __loader_dlopen out of the linker. */
    void *from_disk = eclipse_elf_lookup_symbol(path, base, "eclipse_fixture_marker");
    CHECK(from_disk == expected,
          "ELF lookup gave %p, dlsym gave %p", from_disk, expected);

    CHECK(eclipse_elf_lookup_symbol(path, base, "no_such_function_here") == NULL,
          "an unknown symbol must not resolve");

    /* Address lookup: which mapping owns this one. The test binary's own code
     * is file-backed, so the answer must name this executable. */
    char owner[512];
    owner[0] = '\0';
    CHECK(eclipse_maps_path_at((uintptr_t)&test_maps_and_elf, owner, sizeof(owner)),
          "the address of a function in this file must be mapped");
    CHECK(strstr(owner, "test_eclipseexec") != NULL,
          "the owner should be this executable, got \"%s\"", owner);

    /* SONAME patching: a shorter replacement is written in place. */
    int fd = eclipse_elf_clone_with_soname(path, "libclipse.so", "/tmp");
    CHECK(fd >= 0, "clone_with_soname failed: %s", strerror(errno));
    if (fd >= 0) {
        size_t length = 0;
        char *image = read_all(fd, &length);
        CHECK(image != NULL, "reading the patched copy");
        if (image != NULL) {
            CHECK(length > 0, "the patched copy is not empty");
            CHECK(memmem(image, length, "libclipse.so", strlen("libclipse.so")) != NULL,
                  "the new SONAME should be in the copy");
            CHECK(memmem(image, length, "libfixture.so", strlen("libfixture.so")) == NULL,
                  "the old SONAME should be gone from the copy");
            free(image);
        }
        close(fd);
    }

    /* A longer replacement cannot fit; it must be refused, not overflow. */
    int fd_long = eclipse_elf_clone_with_soname(path, "libconsiderably_longer.so", "/tmp");
    CHECK(fd_long < 0, "a SONAME longer than the original must be refused");
    if (fd_long >= 0) {
        close(fd_long);
    }

    CHECK(eclipse_elf_clone_with_soname("/nonexistent/whatever.so", "libclipse.so", "/tmp") < 0,
          "a missing source file must be refused");

    dlclose(loaded);
}

/* ---- entry -------------------------------------------------------------- */

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <path to libfixture.so>\n", argv[0]);
        return 2;
    }

    test_bypass_before_library_dir();
    test_configuration();
    test_egl_paths();
    test_affinity();
    test_maps_and_elf(argv[1]);

    if (failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }

    printf("eclipseexec host tests: all checks passed\n");
    return 0;
}
