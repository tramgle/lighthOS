/* sleep N — poll SYS_TIME (100 Hz ticks) until N seconds elapse. */
#include "ulib_x64.h"

int main(int argc, char **argv, char **envp) {
    (void)envp;
    if (argc < 2) return 0;
    long secs = u_atoi(argv[1]);
    long deadline = sys_time() + secs * 100;
    while (sys_time() < deadline) sys_yield();
    return 0;
}
