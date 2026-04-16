/* L5 pilot user binary. Replaces user/hello_l4.s with a real C
   program linked against the new x86_64 crt0. */

#include "syscall_x64.h"

static void put_dec(long v) {
    char buf[24]; int i = 0;
    if (v == 0) { uputs("0"); return; }
    if (v < 0) { uputs("-"); v = -v; }
    while (v > 0) { buf[i++] = '0' + (v % 10); v /= 10; }
    while (i > 0) { char c = buf[--i]; sys_write(1, &c, 1); }
}

int main(int argc, char **argv, char **envp) {
    (void)envp;
    uputs("hello from x86_64 user space\n");
    uputs("  pid  = "); put_dec(sys_getpid()); uputs("\n");
    uputs("  argc = "); put_dec(argc);         uputs("\n");
    for (int i = 0; i < argc; i++) {
        uputs("  argv[");
        put_dec(i);
        uputs("] = ");
        uputs(argv[i] ? argv[i] : "(null)");
        uputs("\n");
    }
    return 42;
}
