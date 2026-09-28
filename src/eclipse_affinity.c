/*
 * eclipseexec — big-core affinity.
 * Copyright (c) 2026 Shadow. SPDX-License-Identifier: MIT
 *
 * Minecraft's render thread does not care how many cores the device has; it
 * wants the fastest one. This finds the CPU with the highest advertised clock
 * in /sys and pins the calling thread to it, once per thread.
 *
 * The pinning is on by default: calling eclipseexec_make_bigcore_affine() is
 * itself the request. A launcher that disagrees says so with
 * eclipseexec_set_affinity_enabled(false).
 */

#define ECLIPSE_LOG_TAG "EclipseExec/affinity"
#include "eclipseexec.h"
#include "eclipse_log.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_enabled = true;
static bool g_resolved;
static cpu_set_t g_big_cores;

/* Upper bound, not a count: the loop stops after a run of absent
 * directories, which is how the end of the CPU list shows up. */
#define ECLIPSE_MAX_CPUS 256
/* Some SoCs leave a hole in the numbering for a hotplugged cluster. */
#define ECLIPSE_MAX_MISSES 8
#define ECLIPSE_FREQ_BUFFER 32

void eclipseexec_set_affinity_enabled(bool enable)
{
    pthread_mutex_lock(&g_lock);
    g_enabled = enable;
    pthread_mutex_unlock(&g_lock);

    ECLIPSE_LOGI("big-core affinity %s", enable ? "enabled" : "disabled");
}

bool eclipseexec_affinity_enabled(void)
{
    pthread_mutex_lock(&g_lock);
    bool enabled = g_enabled;
    pthread_mutex_unlock(&g_lock);
    return enabled;
}

/* Read cpuinfo_max_freq for one CPU. False when the file is absent, empty or
 * holds something that is not a number. */
static bool read_max_freq(unsigned int cpu, unsigned long *out_hz)
{
    char path[160];
    snprintf(path, sizeof(path),
             "/sys/devices/system/cpu/cpu%u/cpufreq/cpuinfo_max_freq", cpu);

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }

    char buffer[ECLIPSE_FREQ_BUFFER];
    ssize_t count = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);
    if (count <= 0) {
        return false;
    }
    buffer[count] = '\0';

    char *end = NULL;
    unsigned long hz = strtoul(buffer, &end, 10);
    if (end == buffer || hz == 0) {
        return false;
    }

    *out_hz = hz;
    return true;
}

/* Called with g_lock held. */
static bool resolve_big_cores_locked(void)
{
    if (g_resolved) {
        return true;
    }

    unsigned long best_hz = 0;
    unsigned int best_cpu = 0;
    bool found = false;
    unsigned int misses = 0;

    for (unsigned int cpu = 0; cpu < ECLIPSE_MAX_CPUS && misses < ECLIPSE_MAX_MISSES; cpu++) {
        unsigned long hz = 0;
        if (!read_max_freq(cpu, &hz)) {
            misses++;
            continue;
        }
        misses = 0;
        if (!found || hz >= best_hz) {
            best_hz = hz;
            best_cpu = cpu;
            found = true;
        }
    }

    if (!found) {
        eclipseexec_set_error("no cpufreq data in /sys for any CPU; "
                              "cannot tell which core is the fast one");
        return false;
    }

    CPU_ZERO(&g_big_cores);
    CPU_SET(best_cpu, &g_big_cores);
    g_resolved = true;

    ECLIPSE_LOGI("fastest CPU is cpu%u at %lu kHz", best_cpu, best_hz / 1000);
    return true;
}

void eclipseexec_make_bigcore_affine(void)
{
    /* Each thread pins itself once; the flag is per-thread so the first
     * renderer thread cannot short-circuit every other one. */
    static _Thread_local bool applied = false;
    if (applied) {
        return;
    }

    if (!eclipseexec_affinity_enabled()) {
        ECLIPSE_LOGD("affinity skipped: disabled by the launcher");
        return;
    }

    pthread_mutex_lock(&g_lock);
    bool ready = resolve_big_cores_locked();
    cpu_set_t set = g_big_cores;
    pthread_mutex_unlock(&g_lock);

    if (!ready) {
        return;
    }

    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        eclipseexec_set_error("sched_setaffinity: %s", strerror(errno));
        return;
    }

    applied = true;
    ECLIPSE_LOGD("thread pinned to the big core");
}
