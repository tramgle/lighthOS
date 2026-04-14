#include "kernel/tss.h"
#include "kernel/gdt.h"
#include "lib/string.h"
#include "lib/kprintf.h"

static tss_entry_t tss;

extern void tss_flush(void);

void tss_init(uint32_t kernel_ss, uint32_t kernel_esp) {
    memset(&tss, 0, sizeof(tss));
    tss.ss0  = kernel_ss;
    tss.esp0 = kernel_esp;
    tss.iomap_base = sizeof(tss_entry_t);

    /* Install TSS descriptor in GDT entry 5 (selector 0x28) */
    uint32_t base  = (uint32_t)&tss;
    uint32_t limit = sizeof(tss_entry_t) - 1;

    /* Access: P=1, DPL=0, S=0 (system), Type=0x9 (32-bit TSS, not busy) = 0x89 */
    gdt_set_entry(5, base, limit, 0x89, 0x00);

    tss_flush();
    serial_printf("[tss] Initialized, esp0=0x%x\n", kernel_esp);
}

void tss_set_kernel_stack(uint32_t esp) {
    tss.esp0 = esp;
}
