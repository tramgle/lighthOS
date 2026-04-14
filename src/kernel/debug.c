#include "kernel/debug.h"
#include "lib/kprintf.h"

void debug_backtrace(uint32_t start_ebp) {
    uint32_t bp = start_ebp;

    serial_printf("Backtrace:\n");
    for (int i = 0; i < 16; i++) {
        if (bp < 0x1000 || bp >= 0x01000000) break;
        if (bp & 3) break;

        uint32_t saved_ebp = ((uint32_t *)bp)[0];
        uint32_t ret_addr  = ((uint32_t *)bp)[1];

        serial_printf("  #%d  ebp=0x%x  eip=0x%x\n", i, bp, ret_addr);

        if (saved_ebp <= bp) break;  /* frame pointer must climb */
        bp = saved_ebp;
    }
}
