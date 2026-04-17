/* x86_64 GDT.
 *
 * Layout chosen to satisfy SYSCALL/SYSRET's selector math. On
 * sysretq the CPU sets CS = STAR[63:48]+16 | 3 and SS =
 * STAR[63:48]+8 | 3, so user data must sit 8 bytes above the STAR
 * base and user 64-bit code 16 bytes above. With STAR[63:48] = 0x18:
 *
 *   0x00  null
 *   0x08  kernel code  (L=1, DPL=0)  0x00AF9A000000FFFF
 *   0x10  kernel data  (DPL=0)       0x00CF92000000FFFF
 *   0x18  user 32-bit code           0x00CFFA000000FFFF  (unused placeholder)
 *   0x20  user data    (DPL=3)       0x00CFF2000000FFFF  → SS at sysretq
 *   0x28  user 64-bit code (L=1,DPL=3) 0x00AFFA000000FFFF → CS at sysretq
 *   0x30  TSS low half               (filled by gdt_install_tss)
 *   0x38  TSS high half
 */

#include "kernel/gdt.h"
#include "kernel/tss.h"
#include "lib/string.h"
#include "lib/kprintf.h"

static uint64_t gdt[8] __attribute__((aligned(8)));

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct gdt_ptr gdtp;

extern void gdt_flush(struct gdt_ptr *ptr);

void gdt_init(void) {
    memset(gdt, 0, sizeof(gdt));
    gdt[0] = 0x0000000000000000ULL;
    gdt[1] = 0x00AF9A000000FFFFULL;         /* kernel code (0x08)    */
    gdt[2] = 0x00CF92000000FFFFULL;         /* kernel data (0x10)    */
    gdt[3] = 0x00CFFA000000FFFFULL;         /* user 32-bit code (0x18, placeholder) */
    gdt[4] = 0x00CFF2000000FFFFULL;         /* user data (0x20)      */
    gdt[5] = 0x00AFFA000000FFFFULL;         /* user 64-bit code (0x28) */
    /* gdt[6..7] = TSS — filled by gdt_install_tss from tss_init. */

    gdtp.limit = sizeof(gdt) - 1;
    gdtp.base  = (uint64_t)(uintptr_t)&gdt[0];

    gdt_flush(&gdtp);
    serial_printf("[gdt] loaded @0x%lx (%u bytes)\n",
                  (uint64_t)(uintptr_t)&gdt[0], (uint32_t)sizeof(gdt));
}

void gdt_install_tss(uint64_t base, uint32_t limit) {
    uint64_t low = 0;
    low |= (uint64_t)(limit & 0xFFFF);
    low |= ((uint64_t)(limit >> 16) & 0xF) << 48;
    low |= (base & 0xFFFFFFULL) << 16;
    low |= ((base >> 24) & 0xFFULL) << 56;
    low |= (uint64_t)0x89 << 40;             /* P=1 DPL=0 type=9 (TSS avail) */
    /* flags nibble = 0 (byte granularity — TSS limit is in bytes) */

    uint64_t high = (base >> 32) & 0xFFFFFFFFULL;

    gdt[6] = low;
    gdt[7] = high;
}
