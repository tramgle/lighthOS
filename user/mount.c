/* mount [-t type] <source> <mountpoint> [flags] — no list mode. */
#include "ulib_x64.h"

int main(int argc, char **argv, char **envp) {
    (void)envp;
    const char *type = "fat";
    int arg = 1;
    if (arg + 1 < argc && u_strcmp(argv[arg], "-t") == 0) {
        type = argv[arg + 1];
        arg += 2;
    }
    if (argc - arg < 2) { u_puts_n("mount: usage: [-t type] src mountpoint [flags]\n"); return 2; }
    const char *src = argv[arg];
    const char *mp  = argv[arg + 1];
    const char *flags = (arg + 2 < argc) ? argv[arg + 2] : "rw";
    return sys_mount(src, mp, type, flags) == 0 ? 0 : 1;
}
