/* umount TARGET — detach the filesystem mounted at TARGET. The
   detached fs's in-memory nodes stay around (kernel doesn't garbage-
   collect them yet), but routing through TARGET stops. */

#include "syscall.h"
#include "ulib.h"

int main(int argc, char **argv) {
    if (argc != 2) {
        puts("usage: umount TARGET\n");
        return 1;
    }
    if (sys_umount(argv[1]) != 0) {
        printf("umount: failed on %s\n", argv[1]);
        return 1;
    }
    return 0;
}
