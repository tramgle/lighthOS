/* dyn_echo — dynamic-linking pilot for libvibc.so.1.
   Uses libvibc's snprintf (which itself calls ulib's strlen) to prove
   the DT_NEEDED libvibc → libulib chain resolves. Each argv entry
   goes to stdout, space-separated, newline-terminated. */

#include "syscall.h"
#include "ulib.h"
#include <stdio.h>   /* libvibc snprintf declaration */

int main(int argc, char **argv) {
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
