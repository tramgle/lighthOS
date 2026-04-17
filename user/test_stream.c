/* test_stream: verify sys_write returns the byte count given,
   partial writes aren't a thing today, and a long buffer goes
   through intact. */
#include "syscall_x64.h"

int main(int argc, char **argv, char **envp) {
    (void)argc; (void)argv; (void)envp;
    char buf[256];
    for (int i = 0; i < (int)sizeof(buf); i++) buf[i] = '.';
    buf[sizeof(buf) - 1] = '\n';
    if (sys_write(1, buf, sizeof(buf)) != (long)sizeof(buf)) return 40;
    /* getpid + time are cheap read-only syscalls — stress a few. */
    for (int i = 0; i < 32; i++) {
        if (sys_getpid() <= 0) return 41;
        if (sys_time() < 0)    return 42;
    }
    return 0;
}
