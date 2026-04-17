#ifndef TSS_H
#define TSS_H

#include "include/types.h"

/* 64-bit TSS — 104 bytes, packed, no segment registers. */
typedef struct {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];                /* IST1..IST7 */
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed)) tss_entry_t;

/* Install the TSS, set rsp0 (stack used on CPL transition to 0),
   point IST1 at a fault stack for #DF / #PF / #NMI, and run `ltr`. */
void tss_init(uint64_t rsp0);
void tss_set_kernel_stack(uint64_t rsp);

/* Provided by gdt.c — installs the 16-byte TSS descriptor at
   GDT[5..6] using the TSS base from .bss. */
void gdt_install_tss(uint64_t base, uint32_t limit);

#endif
