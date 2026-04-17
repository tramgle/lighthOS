/* dyn_echo — joins argv with spaces. Dynamic-linked against
 * libvibc.so.1 (for snprintf) + libulib.so.1 (transitive).
 * Exercises recursive DT_NEEDED resolution. */
#include "ulib.h"
#include <stdio.h>

int main(int argc, char **argv, char **envp) {
    (void)envp;
    char buf[256];
    int pos = 0;
    for (int i = 1; i < argc; i++) {
        if (i > 1 && pos < (int)sizeof(buf) - 1) buf[pos++] = ' ';
        int n = snprintf(buf + pos, sizeof(buf) - pos, "%s", argv[i]);
        if (n < 0) break;
        pos += n;
        if (pos >= (int)sizeof(buf) - 1) break;
    }
    if (pos < (int)sizeof(buf) - 1) buf[pos++] = '\n';
    sys_write(1, buf, pos);
    return 0;
}
