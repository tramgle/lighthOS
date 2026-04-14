#include "syscall.h"
#include "ulib.h"

int main(void) {
    printf("NAME      SIZE(KB)  MOUNT         FS       ACCESS\n");
    printf("----      --------  --------      ------   ------\n");

    struct blkdev_info info;
    for (uint32_t i = 0; sys_blkdevs(i, &info) == 0; i++) {
        uint32_t kb = (info.total_sectors / 2);   /* 512-byte sectors */
        const char *mount = info.mount_path[0] ? info.mount_path : "-";
        const char *fs    = info.fs_type[0]    ? info.fs_type    : "-";
        const char *mode  = info.mount_path[0] ? (info.read_only ? "ro" : "rw") : "-";
        printf("%-8s  %8u  %-12s  %-6s   %s\n",
               info.name, kb, mount, fs, mode);
    }
    return 0;
}
