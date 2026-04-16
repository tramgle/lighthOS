/* Minimal x86_64 syscall dispatcher.
 *
 * Entry is INT 0x80 for now (still simpler to debug than SYSCALL
 * MSRs, and user-ABI-compatible with the bringup path). Args come
 * out of registers_t as rdi/rsi/rdx/r10/r8/r9; return via rax.
 *
 * Ports just the syscalls the L5 userland needs:
 *   1  exit      (rdi=code)
 *   3  read      (rdi=fd, rsi=buf, rdx=count)
 *   4  write     (rdi=fd, rsi=buf, rdx=count)
 *   7  waitpid   (rdi=pid, rsi=status_ptr)
 *  20  getpid
 *  24  yield
 * 120  spawn     (rdi=path/name-unused, rsi=argv)
 * 201  shutdown
 *
 * fork/open/close/stat/readdir/etc. return -1 pending their
 * subsystem ports in follow-up sessions.
 */

#include "kernel/syscall.h"
#include "kernel/isr.h"
#include "kernel/process.h"
#include "kernel/task.h"
#include "lib/kprintf.h"

#define SYS_EXIT     1
#define SYS_READ     3
#define SYS_WRITE    4
#define SYS_WAITPID  7
#define SYS_GETPID  20
#define SYS_YIELD   24
#define SYS_FORK    57
#define SYS_SPAWN  120
#define SYS_SHUTDOWN 201

static ssize_t sys_write_console(uint64_t fd, const char *buf, uint64_t n) {
    if (fd != 1 && fd != 2) return -1;
    for (uint64_t i = 0; i < n; i++) {
        char c = buf[i];
        if (c == '\n') { extern void serial_putchar(char); serial_putchar('\r'); }
        extern void serial_putchar(char);
        serial_putchar(c);
    }
    return (ssize_t)n;
}

static void acpi_shutdown(void) {
    /* QEMU-specific shutdown via port 0x604. */
    __asm__ volatile ("outw %0, %1" :: "a"((uint16_t)0x2000), "Nd"((uint16_t)0x604));
    for (;;) __asm__ volatile ("hlt");
}

static registers_t *syscall_handler(registers_t *regs) {
    uint64_t num = regs->rax;
    uint64_t a1 = regs->rdi;
    uint64_t a2 = regs->rsi;
    uint64_t a3 = regs->rdx;
    (void)a3;

    switch (num) {
    case SYS_EXIT:
        process_exit((int)a1);
        regs->rax = 0;
        break;
    case SYS_WRITE:
        regs->rax = (uint64_t)sys_write_console(a1, (const char *)(uintptr_t)a2, a3);
        break;
    case SYS_READ:
        regs->rax = 0;                      /* EOF on console for now */
        break;
    case SYS_GETPID: {
        process_t *p = process_current();
        regs->rax = p ? p->pid : 0;
        break;
    }
    case SYS_YIELD:
        /* yield will happen on return through schedule() */
        regs->rax = 0;
        break;
    case SYS_FORK:
        regs->rax = (uint64_t)(int64_t)process_fork(regs);
        break;
    case SYS_WAITPID: {
        int status = 0;
        int r = process_waitpid((uint32_t)a1, &status);
        if (a2) *(int *)(uintptr_t)a2 = status;
        regs->rax = (uint64_t)r;
        break;
    }
    case SYS_SHUTDOWN:
        kprintf("[kernel] shutdown requested by pid %u\n",
                process_current() ? process_current()->pid : 0);
        acpi_shutdown();
        break;
    default:
        kprintf("[syscall] unknown %lu from pid %u\n", num,
                process_current() ? process_current()->pid : 0);
        regs->rax = (uint64_t)-1;
        break;
    }
    return regs;
}

void syscall_init(void) {
    isr_register_handler(0x80, syscall_handler);
}
