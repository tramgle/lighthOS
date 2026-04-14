#include "syscall.h"
#include "ulib.h"

int main(void) {
    printf("PMM regions:\n");
    struct region_info r;
    for (uint32_t i = 0; sys_regions(i, &r) == 0; i++) {
        uint32_t frames = (r.end_addr - r.start_addr) / 4096;
        printf("  0x%x - 0x%x  %s  %u frames (%u KB)\n",
               r.start_addr, r.end_addr,
               r.used ? "USED" : "FREE",
               frames, (frames * 4096) / 1024);
    }
    return 0;
}
