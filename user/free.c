/* free — report physical memory totals as kibibytes. */

#include <stdio.h>
#include "ulib_x64.h"

int main(int argc, char **argv, char **envp) {
    (void)argc; (void)argv; (void)envp;
    struct meminfo m;
    if (sys_meminfo(&m) != 0) {
        printf("free: syscall failed\n");
        return 1;
    }
    uint64_t used_kb = (m.total_kb > m.free_kb) ? (m.total_kb - m.free_kb) : 0;
    printf("              total        used        free\n");
    printf("Mem:   %10llu  %10llu  %10llu\n",
           (unsigned long long)m.total_kb,
           (unsigned long long)used_kb,
           (unsigned long long)m.free_kb);
    return 0;
}
