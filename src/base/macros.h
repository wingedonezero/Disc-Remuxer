/*
 * Small macros every module uses.
 */

#ifndef DISC_BASE_MACROS_H
#define DISC_BASE_MACROS_H

#define DR_MIN(a, b) ((a) < (b) ? (a) : (b))
#define DR_MAX(a, b) ((a) > (b) ? (a) : (b))
#define DR_ARRAY_ELEMS(a) (sizeof(a) / sizeof((a)[0]))

#endif
