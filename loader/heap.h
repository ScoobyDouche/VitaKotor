/* heap.h -- newlib heap telemetry and exhaustion handling (see heap.c) */

#ifndef __HEAP_H__
#define __HEAP_H__

#include <stddef.h>

// Arm the C++ new-handler. Call once, early.
void heap_init(void);

// Log one line of heap occupancy. `why` labels the line; NULL for the periodic
// sample.
void heap_log(const char *why);

// Size of the most recent `new` the game made through our dynlib table, so the
// handler can say whether the failing request was large or the heap was simply
// full. Written on every new, read only when one fails.
extern volatile unsigned g_heap_last_new;

#endif
