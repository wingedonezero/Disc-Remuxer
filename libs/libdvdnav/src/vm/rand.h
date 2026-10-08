/*
 * Copyright (C) 2026 Disc-Remuxer contributors
 *
 * This file is part of libdvdnav, a DVD navigation library.
 *
 * libdvdnav is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * libdvdnav is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with libdvdnav; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef LIBDVDNAV_RAND_H
#define LIBDVDNAV_RAND_H

#include <stdint.h>

/*
 * Deterministic random program order for program chains with random or
 * shuffle playback (pg_playback_mode != 0).
 *
 * The generator is the "minimal standard" multiplicative congruential
 * generator x' = x * 48271 mod (2^31 - 1). Its state is seeded from fields
 * of the VMGI, so a disc always plays its random program chains in the same
 * order, and copies of a navigator continue the same sequence.
 */

/* Largest program count vm_rand_shuffle() handles (program numbers are 8 bit) */
#define VM_SHUFFLE_MAX 288

/* Advances the state by one step and returns the new state (1..2^31-2 for
 * a state in that range). */
uint32_t vm_rand_next(uint32_t *state);

/* Mixes a value into the state: state = next(((state + value) | 0x8000)
 * & 0x7fffffff), all 32-bit. */
void vm_rand_seed(uint32_t *state, uint32_t value);

/*
 * Element "step" (0-based) of the random order of 0..bound-1 that belongs to
 * the given state; the state itself is not changed. Steps 0..bound-1 form
 * one permutation, each further run of bound steps a new one. With 4 or more
 * elements a permutation never starts with 0 (program 1).
 * Returns 0 when bound >= VM_SHUFFLE_MAX, -1 when bound is 0.
 */
int vm_rand_shuffle(uint32_t state, unsigned int bound, unsigned int step);

#endif /* LIBDVDNAV_RAND_H */
