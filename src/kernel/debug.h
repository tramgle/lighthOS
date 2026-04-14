#ifndef DEBUG_H
#define DEBUG_H

#include "include/types.h"
#include "lib/kprintf.h"

#ifndef DEBUG_KERNEL
#define DEBUG_KERNEL 0
#endif

/* Conditional verbose logging. Use for per-op chatter ("created task N",
   "loaded segment", etc.) that's noise in steady state but invaluable
   while chasing a regression. Boot banners and errors should stay as
   unconditional serial_printf. */
#if DEBUG_KERNEL
#define dlog(...) serial_printf(__VA_ARGS__)
#else
#define dlog(...) ((void)0)
#endif

/* Walk ebp chain starting at `start_ebp`, printing up to 16 frames to
   serial. Safe against unmapped/garbage frame pointers — stops when bp
   leaves a sensible kernel range or fails alignment. */
void debug_backtrace(uint32_t start_ebp);

#endif
