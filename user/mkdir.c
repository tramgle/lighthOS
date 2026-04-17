/* mkdir <dir>... */
#include "ulib_x64.h"

int main(int argc, char **argv, char **envp) {
    (void)envp;
    int rc = 0;
    for (int i = 1; i < argc; i++)
        if (sys_mkdir(argv[i]) != 0) rc = 1;
    return rc;
}
