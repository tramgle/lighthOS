/* ps — list live processes. One line per process with pid, parent,
   pgid, state, and command name. */

#include <stdio.h>
#include "ulib_x64.h"

static const char *state_name(uint32_t s) {
    switch (s) {
    case 0: return "READY  ";
    case 1: return "RUN    ";
    case 2: return "BLOCKED";
    case 3: return "STOPPED";
    case 4: return "DEAD   ";
    default: return "?      ";
    }
}

int main(int argc, char **argv, char **envp) {
    (void)argc; (void)argv; (void)envp;

    printf("  PID  PPID  PGID  STATE    NAME\n");
    struct proc_info p;
    for (uint32_t i = 0; sys_ps(i, &p) == 0; i++) {
        printf("%5u %5u %5u  %s  %s\n",
               p.pid, p.parent_pid, p.pgid, state_name(p.state), p.name);
    }
    return 0;
}
