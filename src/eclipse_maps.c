/*
 * eclipseexec — /proc/self/maps lookup.
 * Copyright (c) 2026 Shadow. SPDX-License-Identifier: MIT
 */

#define ECLIPSE_LOG_TAG "EclipseExec/maps"
#include "eclipse_maps.h"
#include "eclipse_log.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

struct mapping {
    uintptr_t start;
    uintptr_t end;
    char perms[5];
    char path[512];
};

/* One /proc/self/maps line: "start-end perms offset device inode path". */
static bool parse_line(const char *line, struct mapping *out)
{
    unsigned long start = 0, end = 0, offset = 0, inode = 0;
    unsigned int major = 0, minor = 0;
    char perms[5] = {0};
    char path[512] = {0};

    int fields = sscanf(line, "%lx-%lx %4s %lx %x:%x %lu %511[^\n]",
                        &start, &end, perms, &offset, &major, &minor, &inode, path);
    if (fields < 7) {
        return false;
    }

    out->start = (uintptr_t)start;
    out->end = (uintptr_t)end;
    memcpy(out->perms, perms, sizeof(perms));
    if (fields >= 8) {
        strncpy(out->path, path, sizeof(out->path) - 1);
    }
    out->path[sizeof(out->path) - 1] = '\0';
    return true;
}

bool eclipse_maps_find(const char *needle, char *out_path, size_t path_cap, uintptr_t *out_base)
{
    if (needle == NULL || out_base == NULL) {
        return false;
    }

    FILE *fp = fopen("/proc/self/maps", "r");
    if (fp == NULL) {
        ECLIPSE_LOGE("cannot read /proc/self/maps: %s", strerror(errno));
        return false;
    }

    bool found = false;
    struct mapping map;
    char line[768];

    while (fgets(line, sizeof(line), fp) != NULL) {
        memset(&map, 0, sizeof(map));
        if (!parse_line(line, &map)) {
            continue;
        }

        /* Only file-backed, readable mappings. Anonymous regions, [stack] and
         * [vdso] never hold the library we are hunting for. */
        if (map.path[0] != '/' || map.perms[0] != 'r') {
            continue;
        }
        if (strstr(map.path, needle) == NULL) {
            continue;
        }

        if (out_path != NULL && path_cap > 0) {
            strncpy(out_path, map.path, path_cap - 1);
            out_path[path_cap - 1] = '\0';
        }
        /* The file is ordered by address, so the first readable match is the
         * lowest mapping of the file: its load base. */
        *out_base = map.start;
        found = true;
        break;
    }

    fclose(fp);
    return found;
}

bool eclipse_maps_path_at(uintptr_t address, char *out_path, size_t path_cap)
{
    if (out_path == NULL || path_cap == 0) {
        return false;
    }
    out_path[0] = '\0';

    FILE *fp = fopen("/proc/self/maps", "r");
    if (fp == NULL) {
        ECLIPSE_LOGE("cannot read /proc/self/maps: %s", strerror(errno));
        return false;
    }

    bool found = false;
    struct mapping map;
    char line[768];

    while (fgets(line, sizeof(line), fp) != NULL) {
        memset(&map, 0, sizeof(map));
        if (!parse_line(line, &map)) {
            continue;
        }
        if (address < map.start || address >= map.end) {
            continue;
        }

        strncpy(out_path, map.path, path_cap - 1);
        out_path[path_cap - 1] = '\0';
        found = true;
        break;
    }

    fclose(fp);
    return found;
}
