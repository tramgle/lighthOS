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
#define SYS_OPEN     5
#define SYS_CLOSE    6
#define SYS_WAITPID  7
#define SYS_UNLINK  10
#define SYS_STAT    18
#define SYS_LSEEK   19
#define SYS_GETPID  20
#define SYS_YIELD   24
#define SYS_MKDIR   39
#define SYS_MMAP_ANON  9
#define SYS_MOUNT     21
#define SYS_UMOUNT    22
#define SYS_ALARM     27
#define SYS_KILL      37
#define SYS_PIPE      42
#define SYS_SIGNAL    48
#define SYS_DUP2      63
#define SYS_FORK      57
#define SYS_SIGRETURN 119
#define SYS_SETPGID  109
#define SYS_GETPGID  108
#define SYS_CHROOT   161
#define SYS_MPROTECT 125

#define SIG_INT    2
#define SIG_KILL   9
#define SIG_ALRM  14
#define SIG_TERM  15
#define SIG_CONT  18
#define SIG_STOP  19
#define SIG_DFL  ((void *)0)
#define SIG_IGN  ((void *)1)

#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4
#define SYS_EXECVE  59
#define SYS_READDIR 89
#define SYS_SPAWN  120
#define SYS_SHUTDOWN 201
#define SYS_TIME    214

#define O_RDONLY 0x00
#define O_WRONLY 0x01
#define O_RDWR   0x02
#define O_CREAT  0x40
#define O_TRUNC  0x200
#define O_APPEND 0x400

#define VFS_FILE 1
#define VFS_DIR  2

struct vfs_stat {
    uint32_t inode;
    uint32_t type;
    uint32_t size;
};

static inline long _syscall0(long num) {
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(num) : "rcx", "r11", "memory");
    return ret;
}
static inline long _syscall1(long num, long a1) {
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(num), "D"(a1) : "rcx", "r11", "memory");
    return ret;
}
static inline long _syscall2(long num, long a1, long a2) {
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(num), "D"(a1), "S"(a2) : "rcx", "r11", "memory");
    return ret;
}
static inline long _syscall3(long num, long a1, long a2, long a3) {
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"(num), "D"(a1), "S"(a2), "d"(a3) : "rcx", "r11", "memory");
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
static inline long sys_open(const char *path, long flags) {
    return _syscall2(SYS_OPEN, (long)(uintptr_t)path, flags);
}
static inline long sys_close(int fd)             { return _syscall1(SYS_CLOSE, fd); }
static inline long sys_stat(const char *path, struct vfs_stat *st) {
    return _syscall2(SYS_STAT, (long)(uintptr_t)path, (long)(uintptr_t)st);
}
static inline long sys_unlink(const char *path)  { return _syscall1(SYS_UNLINK, (long)(uintptr_t)path); }
static inline long sys_mkdir(const char *path)   { return _syscall1(SYS_MKDIR, (long)(uintptr_t)path); }
static inline long sys_lseek(int fd, long off, int whence) {
    return _syscall3(SYS_LSEEK, fd, off, whence);
}
static inline long sys_time(void)                { return _syscall0(SYS_TIME); }

/* strace ring sharable with /bin/strace. */
struct u_strace_entry {
    uint32_t seq, pid, num, exited;
    long a1, a2, a3, a4;
    long ret;
};
#define SYS_TRACEME    231
#define SYS_TRACE_READ 232
static inline long sys_traceme(int pid) { return _syscall1(SYS_TRACEME, pid); }
static inline long sys_trace_read(unsigned seq, struct u_strace_entry *out) {
    return _syscall2(SYS_TRACE_READ, seq, (long)(uintptr_t)out);
}
static inline long sys_execve(const char *path, char *const argv[], char *const envp[]) {
    return _syscall3(SYS_EXECVE, (long)(uintptr_t)path,
                     (long)(uintptr_t)argv, (long)(uintptr_t)envp);
}
static inline long sys_dup2(int oldfd, int newfd) {
    return _syscall2(SYS_DUP2, oldfd, newfd);
}
static inline long sys_pipe(int fds[2]) {
    return _syscall1(SYS_PIPE, (long)(uintptr_t)fds);
}
static inline long sys_mmap_anon(void *addr, size_t len, long prot) {
    return _syscall3(SYS_MMAP_ANON, (long)(uintptr_t)addr, (long)len, prot);
}
static inline long sys_mprotect(void *addr, size_t len, long prot) {
    return _syscall3(SYS_MPROTECT, (long)(uintptr_t)addr, (long)len, prot);
}
/* sys_mount takes 4 args; use _syscall4 which passes args 1..4 in
   RDI, RSI, RDX, R10 per the SysV-syscall convention. */
static inline long _syscall4(long num, long a1, long a2, long a3, long a4) {
    long ret;
    register long r10 __asm__("r10") = a4;
    __asm__ volatile ("syscall"
                      : "=a"(ret)
                      : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
                      : "rcx", "r11", "memory");
    return ret;
}
static inline long sys_mount(const char *src, const char *mp,
                             const char *type, const char *flags) {
    return _syscall4(SYS_MOUNT, (long)(uintptr_t)src, (long)(uintptr_t)mp,
                     (long)(uintptr_t)type, (long)(uintptr_t)flags);
}
static inline long sys_umount(const char *mp) {
    return _syscall1(SYS_UMOUNT, (long)(uintptr_t)mp);
}
static inline long sys_chroot(const char *path) {
    return _syscall1(SYS_CHROOT, (long)(uintptr_t)path);
}
static inline long sys_kill(int pid, int signo) {
    return _syscall2(SYS_KILL, pid, signo);
}
/* Raw sys_signal — kernel takes a handler RIP and just stores it.
 * Returns previous handler (0 = SIG_DFL, 1 = SIG_IGN). */
static inline long sys_signal_raw(int signo, void (*handler)(int)) {
    return _syscall2(SYS_SIGNAL, signo, (long)(uintptr_t)handler);
}
/* Legacy name used by user/ulib.c's signal(); alias to raw. */
static inline long sys_signal(int signo, void (*handler)(int)) {
    return sys_signal_raw(signo, handler);
}
static inline long sys_alarm(unsigned secs) {
    return _syscall1(SYS_ALARM, secs);
}
static inline void sys_sigreturn(void) {
    _syscall0(SYS_SIGRETURN);
    __builtin_unreachable();
}
static inline long sys_setpgid(int pid, int pgid) {
    return _syscall2(SYS_SETPGID, pid, pgid);
}
static inline long sys_getpgid(int pid) {
    return _syscall1(SYS_GETPGID, pid);
}

#define SYS_PS       200
#define SYS_MEMINFO  210
#define SYS_REGIONS  211
#define SYS_PAGEMAP  212
#define SYS_PEEK     213
#define SYS_TTY_RAW   54
#define SYS_TCSETPGRP 55
#define SYS_TTY_WINSZ 56
#define SYS_TTY_POLL  58
#define SYS_VGA_GFX   60
#define SYS_BLKDEVS  215
#define SYS_CHDIR     12
#define SYS_GETCWD   183

static inline long sys_chdir(const char *path) {
    return _syscall1(SYS_CHDIR, (long)(uintptr_t)path);
}
static inline long sys_getcwd(char *buf, size_t cap) {
    return _syscall2(SYS_GETCWD, (long)(uintptr_t)buf, (long)cap);
}

struct proc_info {
    uint32_t pid;
    uint32_t parent_pid;
    uint32_t pgid;
    uint32_t state;
    uint32_t alive;
    char     name[32];
    char     root[256];
};

struct blkdev_info {
    char     name[16];
    uint32_t total_sectors;
    char     mount_path[32];
    char     fs_type[16];
    uint32_t read_only;
};

struct meminfo {
    uint64_t total_kb;
    uint64_t free_kb;
};

struct pagemap_out {
    uint32_t pml4_idx;
    uint32_t pdpt_idx;
    uint32_t pd_idx;
    uint32_t pt_idx;
    uint64_t pml4e;
    uint64_t pdpte;
    uint64_t pde;
    uint64_t pte;
    uint64_t phys;
};

struct region_out {
    uint64_t start_addr;
    uint64_t end_addr;
    uint32_t used;
    uint32_t _pad;
};

static inline long sys_ps(uint32_t idx, struct proc_info *out) {
    return _syscall2(SYS_PS, (long)idx, (long)(uintptr_t)out);
}
static inline long sys_blkdevs(uint32_t idx, struct blkdev_info *out) {
    return _syscall2(SYS_BLKDEVS, (long)idx, (long)(uintptr_t)out);
}
static inline long sys_meminfo(struct meminfo *out) {
    return _syscall1(SYS_MEMINFO, (long)(uintptr_t)out);
}
static inline long sys_peek(uint64_t src, void *dst, uint64_t count) {
    return _syscall3(SYS_PEEK, (long)src, (long)(uintptr_t)dst, (long)count);
}
static inline long sys_pagemap(uint64_t va, struct pagemap_out *out) {
    return _syscall2(SYS_PAGEMAP, (long)va, (long)(uintptr_t)out);
}
static inline long sys_regions(uint32_t idx, struct region_out *out) {
    return _syscall2(SYS_REGIONS, (long)idx, (long)(uintptr_t)out);
}
/* Toggle the kernel's cooked-mode serial line discipline.
   0 = default cooked mode (kernel echoes + handles BS).
   1 = raw mode (caller reads bytes verbatim; handles its own echo).
   Ctrl-C / Ctrl-Z still route to the foreground pgid in raw mode. */
static inline long sys_tty_raw(int enable) {
    return _syscall1(SYS_TTY_RAW, enable);
}
/* Hand the terminal's "foreground" process group to pgid. Ctrl-C /
   Ctrl-Z delivered by the kernel go to this group. */
static inline long sys_tcsetpgrp(int pgid) {
    return _syscall1(SYS_TCSETPGRP, pgid);
}
/* Read or write the cached terminal window size. The kernel has no
   way to probe a serial tty on its own — CSI-6n is a user-space
   dance. Defaults to 24x80 until something (stty size, the shell
   startup probe) writes back via sys_tty_setsize. */
static inline long sys_tty_getsize(uint16_t *rows, uint16_t *cols) {
    return _syscall3(SYS_TTY_WINSZ, 0,
                     (long)(uintptr_t)rows, (long)(uintptr_t)cols);
}
static inline long sys_tty_setsize(int rows, int cols) {
    return _syscall3(SYS_TTY_WINSZ, 1, rows, cols);
}
/* Non-blocking input peek: 1 if a byte is pending on the console,
   0 if not. Lets a caller bound the wait when they issued a probe
   the terminal may never answer. */
static inline long sys_tty_poll(void) {
    return _syscall0(SYS_TTY_POLL);
}
/* Switch the console to VGA mode 13h (320x200x256) and alias the
   0xA0000 framebuffer at the caller-supplied user VA (page-aligned).
   Returns the same VA on success, -1 on failure. One-way — text
   mode isn't restored. Pair with sys_shutdown() on exit. */
static inline long sys_vga_gfx(uint64_t user_va) {
    return _syscall1(SYS_VGA_GFX, (long)user_va);
}

static inline size_t ustrlen(const char *s) {
    size_t n = 0; while (s[n]) n++; return n;
}
static inline void uputs(const char *s) {
    sys_write(1, s, ustrlen(s));
}

#endif
