#include "kernel/tss.h"
#include "kernel/gdt.h"
#include "lib/string.h"
#include "lib/kprintf.h"

static tss_entry_t tss __attribute__((aligned(16)));
static uint8_t ist1_stack[16384] __attribute__((aligned(16)));

extern void tss_flush(void);

void tss_init(uint64_t rsp0) {
    memset(&tss, 0, sizeof(tss));
    tss.rsp0       = rsp0;
    tss.ist[0]     = (uint64_t)(uintptr_t)&ist1_stack[sizeof(ist1_stack)];
    tss.iomap_base = sizeof(tss_entry_t);

    gdt_install_tss((uint64_t)(uintptr_t)&tss, sizeof(tss) - 1);
    tss_flush();

    serial_printf("[tss] loaded. rsp0=0x%lx ist1=0x%lx\n",
                  tss.rsp0, tss.ist[0]);
}

void tss_set_kernel_stack(uint64_t rsp) { tss.rsp0 = rsp; }
