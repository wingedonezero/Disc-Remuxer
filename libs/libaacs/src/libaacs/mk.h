/*
 * This file is part of libaacs
 *
 * The media key from a media key block with device keys or processing keys
 * (shared by the Blu-ray and HD DVD paths).
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see
 * <http://www.gnu.org/licenses/>.
 */

#ifndef AACS_MK_H_
#define AACS_MK_H_

#include "util/attributes.h"
#include "file/keydbcfg.h"
#include "mkb.h"

#include <stdint.h>

/* AACS_SUCCESS and the media key in mk, or an AACS_ERROR_* code */
BD_PRIVATE int aacs_calc_mk_dks(MKB *mkb, dk_list *dkl, uint8_t *mk);
BD_PRIVATE int aacs_calc_mk_pks(MKB *mkb, pk_list *pkl, uint8_t *mk);

#endif /* AACS_MK_H_ */
