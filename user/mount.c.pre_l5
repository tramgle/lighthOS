/* mount [-t TYPE] [-r] SOURCE TARGET
   mount                           (list mounted devices)

   Thin wrapper around SYS_MOUNT. With no args, dumps the lsblk view
   of currently mounted devices. TYPE defaults to "fat" since that's
   what all our block devices use right now; -r requests read-only.
   SOURCE is a blkdev name like ata0p0 (see /bin/lsblk). */

#include "syscall.h"
#include "ulib.h"

static void list_mounts(void) {
    struct blkdev_info info;
    for (uint32_t i = 0; sys_blkdevs(i, &info) == 0; i++) {
        if (info.mount_path[0] == '\0') continue;
        printf("%-8s %-10s %-6s %s\n", info.name, info.mount_path,
               info.fs_type, info.read_only ? "ro" : "rw");
    }
}

int main(int argc, char **argv) {
    if (argc == 1) { list_mounts(); return 0; }

    const char *type = "fat";
    const char *flags = "rw";
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-') break;
        if (strcmp(a, "-t") == 0) {
            if (++i >= argc) { puts("mount: -t needs an argument\n"); return 1; }
            type = argv[i];
        } else if (strcmp(a, "-r") == 0) {
            flags = "ro";
        } else {
            printf("mount: unknown flag %s\n", a);
            return 1;
        }
    }

    if (argc - i != 2) {
        puts("usage: mount [-t TYPE] [-r] SOURCE TARGET\n");
        return 1;
    }
    const char *source = argv[i];
    const char *target = argv[i + 1];

    if (sys_mount(source, target, type, flags) != 0) {
        printf("mount: failed to mount %s at %s\n", source, target);
        return 1;
    }
    return 0;
}
