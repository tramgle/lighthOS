#ifndef GDT_H
#define GDT_H

#include "include/types.h"

/* Selectors — stable across the port since user programs encode
   them at their ABI boundary (e.g. IRET frames). Numerical values
   match the i386 layout for source-compat. */
#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_CODE   0x18
#define GDT_USER_DATA   0x20
#define GDT_TSS         0x28

void gdt_init(void);

#endif
