/* strace CMD [args...] — run CMD under the kernel syscall tracer,
   print each recorded entry after the child exits. Single-target
   (one pid at a time globally); starting a second strace clobbers
   the first's ring. Captures are post-dispatch, so entries are
   ordered by syscall completion, not entry. */

#include "syscall.h"
#include "ulib.h"

static const char *name_for(uint32_t num) {
    switch (num) {
    case SYS_EXIT:     return "exit";
    case SYS_READ:     return "read";
    case SYS_WRITE:    return "write";
    case SYS_OPEN:     return "open";
    case SYS_CLOSE:    return "close";
    case SYS_WAITPID:  return "waitpid";
    case SYS_UNLINK:   return "unlink";
    case SYS_CHDIR:    return "chdir";
    case SYS_STAT:     return "stat";
    case SYS_GETPID:   return "getpid";
    case SYS_YIELD:    return "yield";
    case SYS_MKDIR:    return "mkdir";
    case SYS_FORK:     return "fork";
    case SYS_EXECVE:   return "execve";
    case SYS_READDIR:  return "readdir";
    case SYS_SPAWN:    return "spawn";
    case SYS_GETCWD:   return "getcwd";
    case SYS_PS:       return "ps";
    case SYS_SHUTDOWN: return "shutdown";
    case SYS_MEMINFO:  return "meminfo";
    case SYS_REGIONS:  return "regions";
    case SYS_PAGEMAP:  return "pagemap";
    case SYS_PEEK:     return "peek";
    case SYS_TIME:     return "time";
    case SYS_BLKDEVS:  return "blkdevs";
    case SYS_SBRK:     return "sbrk";
    case SYS_LSEEK:    return "lseek";
    case SYS_DUP2:     return "dup2";
    case SYS_PIPE:     return "pipe";
    case SYS_CHROOT:   return "chroot";
    case SYS_KILL:     return "kill";
    case SYS_SETPGID:  return "setpgid";
    case SYS_GETPGID:  return "getpgid";
    case SYS_SIGNAL:   return "signal";
    case SYS_SIGRETURN:return "sigreturn";
    case SYS_MOUNT:    return "mount";
    case SYS_UMOUNT:   return "umount";
    case SYS_ALARM:    return "alarm";
    case SYS_TRACEME:  return "traceme";
    case SYS_TRACE_READ:return "trace_read";
    default:           return "?";
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        puts("usage: strace CMD [args...]\n");
        return 1;
    }

    int pid = sys_fork();
    if (pid < 0) { puts("strace: fork failed\n"); return 1; }
    if (pid == 0) {
        /* Child: enable tracing on self, then become CMD. */
        sys_traceme(sys_getpid());
        sys_execve(argv[1], &argv[1]);
        puts("strace: execve failed\n");
        sys_exit(127);
    }

    /* Parent: wait for child, then drain the ring. The ring is global
       but keyed by pid — drain stops at the first -1 after we've seen
       the child exit. */
    int status;
    sys_waitpid((uint32_t)pid, &status);
    sys_traceme(0);   /* stop capturing */

    for (uint32_t seq = 0; ; seq++) {
        struct strace_entry e;
        if (sys_trace_read(seq, &e) != 0) break;
        printf("[%u] %s(0x%x, 0x%x, 0x%x, 0x%x) = %d\n",
               e.pid, name_for(e.num),
               e.a1, e.a2, e.a3, e.a4, e.ret);
    }
    printf("+++ exited with %d +++\n", status);
    return 0;
}
