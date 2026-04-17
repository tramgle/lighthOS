/* Fork smoke test. Parent forks, both prints pid + exit with
   distinct codes; parent waitpid-reaps the child. */

#include "syscall_x64.h"

static void put_dec(long v) {
    char buf[24]; int i = 0;
    if (v == 0) { uputs("0"); return; }
    if (v < 0) { uputs("-"); v = -v; }
    while (v > 0) { buf[i++] = '0' + (v % 10); v /= 10; }
    while (i > 0) { char c = buf[--i]; sys_write(1, &c, 1); }
}

int main(int argc, char **argv, char **envp) {
    (void)argc; (void)argv; (void)envp;
    long pre = 0x4242;
    uputs("parent pid = "); put_dec(sys_getpid()); uputs("\n");
    uputs("  pre-fork marker = "); put_dec(pre); uputs("\n");

    long r = sys_fork();
    if (r == 0) {
        uputs("child says: pid = "); put_dec(sys_getpid());
        uputs("  marker = "); put_dec(pre); uputs("\n");
        return 7;
    }
    uputs("parent: fork returned "); put_dec(r); uputs("\n");
    int status = 0;
    long w = sys_waitpid((int)r, &status);
    uputs("parent: waitpid = "); put_dec(w);
    uputs("  status = ");          put_dec(status); uputs("\n");
    return 0;
}
