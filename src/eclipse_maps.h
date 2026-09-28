/*
 * eclipseexec — /proc/self/maps lookup.
 * Copyright (c) 2026 Shadow. SPDX-License-Identifier: MIT
 */
#ifndef ECLIPSE_MAPS_H
#define ECLIPSE_MAPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Find the first readable mapping whose path contains `needle` and is not an
 * anonymous region.
 *
 * @param out_path  receives the path as it appears in the mapping. May be NULL
 *                  if only the base address is wanted.
 * @param path_cap  size of out_path, including the terminator.
 * @param out_base  receives the lowest address of the mapping.
 * @return true when a match was found.
 */
bool eclipse_maps_find(const char *needle, char *out_path, size_t path_cap, uintptr_t *out_base);

/**
 * Find the mapping that contains `address`.
 *
 * @param out_path  receives the path backing the mapping; the empty string for
 *                  an anonymous region.
 * @param path_cap  size of out_path, including the terminator.
 * @return true when the address is mapped.
 */
bool eclipse_maps_path_at(uintptr_t address, char *out_path, size_t path_cap);

#endif /* ECLIPSE_MAPS_H */
