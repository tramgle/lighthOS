/* umount <mountpoint> */
#include "ulib_x64.h"

int main(int argc, char **argv, char **envp) {
    (void)envp;
    if (argc < 2) return 2;
    return sys_umount(argv[1]) == 0 ? 0 : 1;
}
