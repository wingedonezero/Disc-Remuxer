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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>

#include "rand.h"

uint32_t vm_rand_next(uint32_t *state) {
  uint64_t product = (uint64_t)*state * 48271u;
  /* reduce mod 2^31 - 1 using 2^31 = 1 (mod 2^31 - 1), twice */
  uint32_t x = (uint32_t)((product & 0x7fffffff) + (product >> 31));
  x = (x & 0x7fffffff) + (x >> 31);
  *state = x;
  return x;
}

void vm_rand_seed(uint32_t *state, uint32_t value) {
  uint32_t mixed = ((*state + value) | 0x8000) & 0x7fffffff;
  *state = vm_rand_next(&mixed);
}

int vm_rand_shuffle(uint32_t state, unsigned int bound, unsigned int step) {
  uint8_t used[VM_SHUFFLE_MAX];
  unsigned int draw, pick = 0;

  if (bound == 0)
    return -1;
  if (bound >= VM_SHUFFLE_MAX)
    return 0;

  /* each full run of bound steps uses the next state */
  while (step >= bound) {
    step -= bound;
    vm_rand_next(&state);
  }

  /* from 4 elements on, pass over states whose first draw would be 0 */
  if (bound >= 4) {
    for (;;) {
      uint32_t peek = state;
      if (vm_rand_next(&peek) % bound != 0)
        break;
      state = peek;
    }
  }

  /* draw without replacement until element "step" is drawn */
  memset(used, 0, bound);
  for (draw = 0; draw <= step; draw++) {
    unsigned int n = vm_rand_next(&state) % (bound - draw);
    for (pick = 0; ; pick++) {
      if (!used[pick] && n-- == 0)
        break;
    }
    used[pick] = 1;
  }
  return (int)pick;
}
