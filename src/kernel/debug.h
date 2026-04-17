#ifndef DEBUG_H
#define DEBUG_H

#include "include/types.h"
#include "lib/kprintf.h"

#ifndef DEBUG_KERNEL
#define DEBUG_KERNEL 0
#endif

#if DEBUG_KERNEL
#define dlog(...) serial_printf(__VA_ARGS__)
#else
#define dlog(...) ((void)0)
#endif

void debug_backtrace(uint64_t start_rbp);

#endif
