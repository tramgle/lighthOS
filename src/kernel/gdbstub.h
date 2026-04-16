#ifndef GDBSTUB_H
#define GDBSTUB_H

#include "include/types.h"
#include "kernel/isr.h"

/* Initialize the gdb transport (COM2 at 0x2F8) and install the
   INT 3 (breakpoint) handler that drops into the stub loop. Safe
   to call before or after scheduling is enabled; the stub is
   polling-only so it doesn't need IRQs. */
void gdbstub_init(void);

/* Called from trap handlers (INT 3 today; extend to GP, UD, etc.
   later) to enter the gdb remote-protocol loop. Blocks until the
   debugger issues `c` (continue) or `s` (single step). `regs` is
   updated in place if gdb wrote registers. */
void gdbstub_enter(registers_t *regs);

/* Emit an INT 3 from current kernel code to break to gdb. Useful as
   a manual hand-rolled breakpoint when debugging boot problems:
   `if (foo_broken) gdb_break();`. */
static inline void gdb_break(void) { __asm__ volatile ("int3"); }

#endif
