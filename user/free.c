#include "syscall.h"
#include "ulib.h"

int main(void) {
    struct meminfo m;
    if (sys_meminfo(&m) != 0) {
        puts("free: syscall failed\n");
        return 1;
    }
    uint32_t used = m.total_frames - m.free_frames;
    printf("Physical memory:\n");
    printf("  Total: %u pages (%u KB)\n", m.total_frames, (m.total_frames * 4096) / 1024);
    printf("  Used:  %u pages (%u KB)\n", used, (used * 4096) / 1024);
    printf("  Free:  %u pages (%u KB)\n", m.free_frames, (m.free_frames * 4096) / 1024);
    printf("Kernel heap:\n");
    printf("  Used:  %u bytes\n", m.heap_used);
    printf("  Free:  %u bytes\n", m.heap_free);
    return 0;
}
