/* x86_64 user syscall wrappers for the L5 port.
 *
 * INT 0x80 entry (SYSCALL-insn path is a future cleanup). Arg
 * convention: RAX = syscall number, args in RDI, RSI, RDX, R10,
 * R8, R9 — the SysV AMD64 syscall convention (distinct from the
 * user-function convention in that the 4th arg is R10, not RCX,
 * because `syscall` itself uses RCX for the return RIP). Since
 * we're using INT 0x80, RCX is undisturbed and we could use
 * either; R10 is chosen so the wrappers also work if we flip to
 * `syscall` later without changing call sites.
 *
 * Only covers the syscalls the L5a kernel implements. Full libc
 * surface area comes back as process/vfs are re-ported.
 */
#ifndef SYSCALL_X64_H
#define SYSCALL_X64_H

#include <stdint.h>
#include <stddef.h>

#define SYS_EXIT     1
#define SYS_READ     3
#define SYS_WRITE    4
#define SYS_WAITPID  7
#define SYS_GETPID  20
#define SYS_YIELD   24
#define SYS_FORK    57
#define SYS_SPAWN  120
#define SYS_SHUTDOWN 201

static inline long _syscall0(long num) {
    long ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(num) : "memory");
    return ret;
}
static inline long _syscall1(long num, long a1) {
    long ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(num), "D"(a1) : "memory");
    return ret;
}
static inline long _syscall2(long num, long a1, long a2) {
    long ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(num), "D"(a1), "S"(a2) : "memory");
    return ret;
}
static inline long _syscall3(long num, long a1, long a2, long a3) {
    long ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(num), "D"(a1), "S"(a2), "d"(a3) : "memory");
    return ret;
}

static inline void sys_exit(int code)          { _syscall1(SYS_EXIT, code); }
static inline long sys_write(int fd, const void *buf, size_t n) {
    return _syscall3(SYS_WRITE, fd, (long)(uintptr_t)buf, (long)n);
}
static inline long sys_read(int fd, void *buf, size_t n) {
    return _syscall3(SYS_READ, fd, (long)(uintptr_t)buf, (long)n);
}
static inline long sys_getpid(void)            { return _syscall0(SYS_GETPID); }
static inline void sys_yield(void)             { _syscall0(SYS_YIELD); }
static inline long sys_waitpid(int pid, int *status) {
    return _syscall2(SYS_WAITPID, pid, (long)(uintptr_t)status);
}
static inline void sys_shutdown(void)          { _syscall0(SYS_SHUTDOWN); }
static inline long sys_fork(void)               { return _syscall0(SYS_FORK); }

static inline size_t ustrlen(const char *s) {
    size_t n = 0; while (s[n]) n++; return n;
}
static inline void uputs(const char *s) {
    sys_write(1, s, ustrlen(s));
}

#endif
