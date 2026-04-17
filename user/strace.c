/* strace <cmd> [args...] — enable kernel syscall tracer on the
 * fork'd child, exec the target, then drain the ring and print
 * one line per entry: `syscall_name(a1, a2, a3) = ret`.
 */
#include "ulib_x64.h"

static const char *name_of(uint32_t n) {
    switch (n) {
    case 1:   return "exit";
    case 3:   return "read";
    case 4:   return "write";
    case 5:   return "open";
    case 6:   return "close";
    case 7:   return "waitpid";
    case 10:  return "unlink";
    case 18:  return "stat";
    case 19:  return "lseek";
    case 20:  return "getpid";
    case 24:  return "yield";
    case 39:  return "mkdir";
    case 9:   return "mmap";
    case 21:  return "mount";
    case 22:  return "umount";
    case 27:  return "alarm";
    case 37:  return "kill";
    case 42:  return "pipe";
    case 48:  return "signal";
    case 57:  return "fork";
    case 59:  return "execve";
    case 63:  return "dup2";
    case 89:  return "readdir";
    case 119: return "sigreturn";
    case 125: return "mprotect";
    case 161: return "chroot";
    case 201: return "shutdown";
    case 214: return "time";
    default:  return "?";
    }
}

static void print_hex(long v) {
    u_puts_n("0x");
    char b[18]; int i = 0;
    unsigned long u = (unsigned long)v;
    if (u == 0) b[i++] = '0';
    while (u) { int d = u & 0xF; b[i++] = d < 10 ? '0'+d : 'a'+d-10; u >>= 4; }
    while (i > 0) u_putc(b[--i]);
}

int main(int argc, char **argv, char **envp) {
    (void)envp;
    if (argc < 2) { u_puts_n("strace: usage: cmd [args...]\n"); return 2; }

    long pid = sys_fork();
    if (pid == 0) {
        sys_traceme((int)sys_getpid());
        sys_execve(argv[1], argv + 1, 0);
        sys_exit(127);
    }
    int status = 0;
    sys_waitpid((int)pid, &status);

    /* Drain the ring. */
    struct u_strace_entry e;
    unsigned seq = 0;
    while (sys_trace_read(seq, &e) == 0) {
        if (e.exited) {
            u_puts_n("+++ exited("); u_putdec(e.ret); u_puts_n(") +++\n");
        } else {
            u_puts_n(name_of(e.num));
            u_puts_n("(");
            print_hex(e.a1);
            u_puts_n(", ");
            print_hex(e.a2);
            u_puts_n(", ");
            print_hex(e.a3);
            u_puts_n(") = ");
            u_putdec(e.ret);
            u_putc('\n');
        }
        seq++;
    }
    return status;
}
