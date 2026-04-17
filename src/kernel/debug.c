#include "kernel/debug.h"
#include "kernel/ksyms.h"
#include "lib/kprintf.h"

void debug_backtrace(uint64_t start_rbp) {
    uint64_t bp = start_rbp;
    kprintf("Backtrace:\n");
    for (int i = 0; i < 16; i++) {
        /* Kernel stack and heap live in the higher half on x86_64,
           so any valid frame pointer sits above the canonical
           low-half ceiling. Cheap sanity check stops backtrace if
           the chain walks into user space or unmapped memory. */
        if ((int64_t)bp >= 0) break;
        if (bp & 7) break;

        uint64_t saved_bp = ((uint64_t *)(uintptr_t)bp)[0];
        uint64_t ret_rip  = ((uint64_t *)(uintptr_t)bp)[1];

        uint64_t off = 0;
        const char *name = ksym_lookup(ret_rip, &off);
        if (name)
            kprintf("  #%d  rip=0x%lx  %s+0x%lx\n", i, ret_rip, name, off);
        else
            kprintf("  #%d  rip=0x%lx  <?>\n", i, ret_rip);

        if (saved_bp <= bp) break;
        bp = saved_bp;
    }
}
