/* lsblk — list block devices registered with the kernel. */

#include <stdio.h>
#include "ulib_x64.h"

int main(int argc, char **argv, char **envp) {
    (void)argc; (void)argv; (void)envp;

    printf("NAME      SIZE(KB)  MOUNT       FS      MODE\n");
    struct blkdev_info info;
    for (uint32_t i = 0; sys_blkdevs(i, &info) == 0; i++) {
        uint32_t kb = info.total_sectors / 2;
        const char *mount = info.mount_path[0] ? info.mount_path : "-";
        const char *fs    = info.fs_type[0]    ? info.fs_type    : "-";
        const char *mode  = info.mount_path[0] ? (info.read_only ? "ro" : "rw") : "-";
        printf("%-8s  %8u  %-10s  %-6s  %s\n",
               info.name, kb, mount, fs, mode);
    }
    return 0;
}
