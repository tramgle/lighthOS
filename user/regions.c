/* regions: dump the PMM's physical-memory region table. */

#include "ulib_x64.h"

int main(int argc, char **argv, char **envp) {
    (void)argc; (void)argv; (void)envp;
    u_puts_n("PMM regions:\n");
    struct region_out r;
    for (uint32_t i = 0; sys_regions(i, &r) == 0; i++) {
        uint64_t bytes = r.end_addr - r.start_addr;
        uint64_t frames = bytes / 4096;
        u_puts_n("  0x"); u_puthex(r.start_addr);
        u_puts_n(" - 0x"); u_puthex(r.end_addr);
        u_putc(' ');
        u_puts_n(r.used ? "USED" : "FREE");
        u_puts_n("  "); u_putdec((long)frames); u_puts_n(" frames (");
        u_putdec((long)(bytes / 1024)); u_puts_n(" KB)\n");
    }
    return 0;
}
