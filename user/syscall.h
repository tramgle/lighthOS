#ifndef USER_SYSCALL_H
#define USER_SYSCALL_H

/* Syscall numbers — must match kernel/syscall.h */
#define SYS_EXIT    1
#define SYS_READ    3
#define SYS_WRITE   4
#define SYS_OPEN    5
#define SYS_CLOSE   6
#define SYS_WAITPID 7
#define SYS_UNLINK  10
#define SYS_CHDIR   12
#define SYS_STAT    18
#define SYS_GETPID  20
#define SYS_YIELD   24
#define SYS_MKDIR   39
#define SYS_FORK    57
#define SYS_EXECVE  59
#define SYS_READDIR 89
#define SYS_SPAWN   120
#define SYS_GETCWD  183
#define SYS_PS      200
#define SYS_SHUTDOWN 201
#define SYS_MEMINFO 210
#define SYS_REGIONS 211
#define SYS_PAGEMAP 212
#define SYS_PEEK    213
#define SYS_TIME    214
#define SYS_BLKDEVS 215
#define SYS_SBRK    45
#define SYS_LSEEK   19
#define SYS_DUP2    63
#define SYS_PIPE    42
#define SYS_CHROOT  161
#define SYS_KILL    37
#define SYS_SETPGID 109
#define SYS_GETPGID 108
#define SYS_SIGNAL    48
#define SYS_SIGRETURN 119
#define SYS_MOUNT     21
#define SYS_UMOUNT    22
#define SYS_ALARM     27
#define SYS_TRACEME     231
#define SYS_TRACE_READ  232
#define SYS_MMAP_ANON   9
#define SYS_MPROTECT    125

/* mmap/mprotect protection flags. PROT_EXEC unenforced (no NX on our paging). */
#define PROT_READ    0x1
#define PROT_WRITE   0x2
#define PROT_EXEC    0x4

/* Signal numbers (Linux-compatible subset). */
#define SIG_HUP   1
#define SIG_INT   2
#define SIG_KILL  9
#define SIG_ALRM 14
#define SIG_TERM 15
#define SIG_CONT 18
#define SIG_STOP 19

/* Keep these in sync with src/fs/vfs.h + src/kernel/process.h. User
   programs pick these up through syscall.h so buffer sizes stay
   consistent with what the kernel accepts. */
#define VFS_MAX_NAME 64
#define VFS_MAX_PATH 256
#define FD_MAX       16

#define O_RDONLY 0x00
#define O_WRONLY 0x01
#define O_RDWR   0x02
#define O_CREAT  0x40
#define O_TRUNC  0x200
#define O_APPEND 0x400

typedef unsigned int uint32_t;
typedef int int32_t;

/* Kept layout-compatible with struct vfs_stat in src/fs/vfs.h — the
   sys_stat syscall writes this exact byte sequence. */
struct vfs_stat {
    uint32_t inode;
    uint32_t type;   /* 1=VFS_FILE, 2=VFS_DIR */
    uint32_t size;
};
#define VFS_FILE 1
#define VFS_DIR  2

static inline int32_t _syscall0(uint32_t num) {
    int32_t ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(num) : "memory");
    return ret;
}

static inline int32_t _syscall1(uint32_t num, uint32_t a1) {
    int32_t ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(num), "b"(a1) : "memory");
    return ret;
}

static inline int32_t _syscall2(uint32_t num, uint32_t a1, uint32_t a2) {
    int32_t ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(num), "b"(a1), "c"(a2) : "memory");
    return ret;
}

static inline int32_t _syscall3(uint32_t num, uint32_t a1, uint32_t a2, uint32_t a3) {
    int32_t ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(num), "b"(a1), "c"(a2), "d"(a3) : "memory");
    return ret;
}

static inline int32_t _syscall4(uint32_t num, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4) {
    int32_t ret;
    __asm__ volatile ("int $0x80" : "=a"(ret) : "a"(num), "b"(a1), "c"(a2), "d"(a3), "S"(a4) : "memory");
    return ret;
}

static inline void sys_exit(int code) { _syscall1(SYS_EXIT, (uint32_t)code); }
static inline int32_t sys_read(int fd, void *buf, uint32_t n) { return _syscall3(SYS_READ, fd, (uint32_t)buf, n); }
static inline int32_t sys_write(int fd, const void *buf, uint32_t n) { return _syscall3(SYS_WRITE, fd, (uint32_t)buf, n); }
static inline int32_t sys_open(const char *path, uint32_t flags) { return _syscall2(SYS_OPEN, (uint32_t)path, flags); }
static inline int32_t sys_close(int fd) { return _syscall1(SYS_CLOSE, fd); }
static inline int32_t sys_stat(const char *path, void *st) { return _syscall2(SYS_STAT, (uint32_t)path, (uint32_t)st); }
static inline int32_t sys_unlink(const char *path) { return _syscall1(SYS_UNLINK, (uint32_t)path); }
static inline int32_t sys_mkdir(const char *path) { return _syscall1(SYS_MKDIR, (uint32_t)path); }
static inline int32_t sys_getpid(void) { return _syscall0(SYS_GETPID); }
static inline void sys_yield(void) { _syscall0(SYS_YIELD); }
/* sys_spawn / sys_spawnve: child's envp comes from the third syscall
   arg (edx). sys_spawn passes the caller's current `environ` so
   in-process setenv/unsetenv propagates to the child (POSIX
   fork+exec semantics). Use sys_spawnve for an explicit env, or
   pass NULL explicitly to inherit the kernel's spawn-time snapshot
   instead of user-space environ. `environ` lives in ulib. */
extern char **environ;
static inline int32_t sys_spawnve(const char *path, char *const argv[],
                                  char *const envp[]) {
    return _syscall3(SYS_SPAWN, (uint32_t)path, (uint32_t)argv, (uint32_t)envp);
}
static inline int32_t sys_spawn(const char *path, char *const argv[]) {
    return sys_spawnve(path, argv, environ);
}
static inline int32_t sys_waitpid(uint32_t pid, int *status) { return _syscall2(SYS_WAITPID, pid, (uint32_t)status); }
static inline int32_t sys_readdir(const char *path, uint32_t idx, char *name, uint32_t *type) { return _syscall4(SYS_READDIR, (uint32_t)path, idx, (uint32_t)name, (uint32_t)type); }
static inline int32_t sys_chdir(const char *path) { return _syscall1(SYS_CHDIR, (uint32_t)path); }
static inline int32_t sys_getcwd(char *buf, uint32_t size) { return _syscall2(SYS_GETCWD, (uint32_t)buf, size); }
static inline int32_t sys_fork(void) { return _syscall0(SYS_FORK); }
static inline int32_t sys_execvee(const char *path, char *const argv[],
                                  char *const envp[]) {
    return _syscall3(SYS_EXECVE, (uint32_t)path, (uint32_t)argv, (uint32_t)envp);
}
static inline int32_t sys_execve(const char *path, char *const argv[]) {
    return sys_execvee(path, argv, environ);
}

#define PROC_NAME_MAX 32
struct proc_info {
    uint32_t pid;
    uint32_t parent_pid;
    uint32_t pgid;
    uint32_t state;
    char     name[PROC_NAME_MAX];
    char     root[VFS_MAX_PATH];
};

static inline int32_t sys_ps(uint32_t idx, struct proc_info *out) { return _syscall2(SYS_PS, idx, (uint32_t)out); }
static inline void    sys_shutdown(void) { _syscall0(SYS_SHUTDOWN); }

struct meminfo {
    uint32_t total_frames;
    uint32_t free_frames;
    uint32_t heap_used;
    uint32_t heap_free;
};
struct region_info {
    uint32_t start_addr;
    uint32_t end_addr;
    uint32_t used;  /* 1 if in use, 0 if free */
};
struct pagemap_info {
    uint32_t pde;
    uint32_t pte;
    uint32_t phys;   /* resolved physical address or 0 */
    uint32_t pd_idx;
    uint32_t pt_idx;
};

static inline int32_t sys_meminfo(struct meminfo *out) { return _syscall1(SYS_MEMINFO, (uint32_t)out); }

struct blkdev_info {
    char     name[32];
    uint32_t total_sectors;
    char     mount_path[32];
    char     fs_type[16];
    uint32_t read_only;
};
static inline int32_t sys_blkdevs(uint32_t idx, struct blkdev_info *out) { return _syscall2(SYS_BLKDEVS, idx, (uint32_t)out); }
static inline int32_t sys_regions(uint32_t idx, struct region_info *out) { return _syscall2(SYS_REGIONS, idx, (uint32_t)out); }
static inline int32_t sys_pagemap(uint32_t vaddr, struct pagemap_info *out) { return _syscall2(SYS_PAGEMAP, vaddr, (uint32_t)out); }
static inline int32_t sys_peek(uint32_t addr, void *buf, uint32_t n) { return _syscall3(SYS_PEEK, addr, (uint32_t)buf, n); }
static inline uint32_t sys_time(void) { return (uint32_t)_syscall0(SYS_TIME); }
/* sys_sbrk(0) returns current brk; sys_sbrk(N) extends it by N bytes
   and returns the OLD break. Returns (void *)-1 on failure. */
static inline void *sys_sbrk(int32_t increment) { return (void *)_syscall1(SYS_SBRK, (uint32_t)increment); }
static inline int32_t sys_lseek(int fd, int32_t offset, int whence) { return _syscall3(SYS_LSEEK, fd, (uint32_t)offset, whence); }
static inline int32_t sys_dup2(int oldfd, int newfd) { return _syscall2(SYS_DUP2, oldfd, newfd); }
/* sys_pipe(fds): fds[0] is the read end, fds[1] is the write end.
   Returns 0 on success, -1 on OOM / fd exhaustion. */
static inline int32_t sys_pipe(int fds[2]) { return _syscall1(SYS_PIPE, (uint32_t)fds); }
static inline int32_t sys_chroot(const char *path) { return _syscall1(SYS_CHROOT, (uint32_t)path); }
/* sys_kill: target > 0 = pid. target < 0 = pgid (absolute value).
   Returns 0 if at least one process matched, -1 otherwise. */
static inline int32_t sys_kill(int target, int signo) { return _syscall2(SYS_KILL, (uint32_t)target, (uint32_t)signo); }
/* sys_setpgid: pid==0 means current; pgid==0 means target's own pid
   (make it a group leader). */
static inline int32_t sys_setpgid(int pid, int pgid) { return _syscall2(SYS_SETPGID, (uint32_t)pid, (uint32_t)pgid); }
static inline int32_t sys_getpgid(int pid) { return _syscall1(SYS_GETPGID, (uint32_t)pid); }

/* Raw signal-handler install. Callers normally go through signal()
   from ulib, which layers a trampoline over this — letting the
   handler just be a plain `void f(int)`. Returns the previous raw
   handler address (0=SIG_DFL, 1=SIG_IGN) on success, -1 on error
   (bad signo, uncatchable signal). */
static inline int32_t sys_signal(int signo, void (*handler)(int)) {
    return _syscall2(SYS_SIGNAL, (uint32_t)signo, (uint32_t)handler);
}

/* sys_sigreturn: never returns — the kernel restores the pre-handler
   register frame and iret's back to the interrupted user instruction. */
static inline void sys_sigreturn(void) {
    _syscall0(SYS_SIGRETURN);
    __builtin_unreachable();
}

/* Mount/umount. source = blkdev name (see /bin/lsblk), target =
   mountpoint path, type = "fat"/"simplefs", flags = "ro"/"rw" (NULL
   defaults to "rw"). */
static inline int32_t sys_mount(const char *source, const char *target,
                                const char *type, const char *flags) {
    return _syscall4(SYS_MOUNT, (uint32_t)source, (uint32_t)target,
                     (uint32_t)type, (uint32_t)flags);
}
static inline int32_t sys_umount(const char *target) {
    return _syscall1(SYS_UMOUNT, (uint32_t)target);
}

/* alarm(N): schedule SIG_ALRM in N seconds (0 = cancel). Returns
   seconds remaining from the previous alarm, or 0 if none was set. */
static inline uint32_t sys_alarm(uint32_t seconds) {
    return (uint32_t)_syscall1(SYS_ALARM, seconds);
}

/* strace support. Single traced pid at a time. sys_traceme(pid) sets
   the target (0 disables). sys_trace_read(seq, *out) copies the seq-th
   captured entry (since trace start) into out; returns -1 past the
   current tail or if that seq has aged out of the ring (size ~1024). */
struct strace_entry {
    uint32_t seq;
    uint32_t pid;
    uint32_t num;
    uint32_t a1, a2, a3, a4;
    int32_t  ret;
};
static inline int32_t sys_traceme(int pid) {
    return _syscall1(SYS_TRACEME, (uint32_t)pid);
}
static inline int32_t sys_trace_read(uint32_t seq, struct strace_entry *out) {
    return _syscall2(SYS_TRACE_READ, seq, (uint32_t)out);
}

/* Anonymous fixed-address mapping. `addr` and `length` must be
   page-aligned (4096). `prot` is a bitmask of PROT_READ / PROT_WRITE
   / PROT_EXEC. Returns `addr` on success, -1 on overlap / OOM /
   out-of-range. Pages are zeroed before being handed back. */
static inline int32_t sys_mmap_anon(uint32_t addr, uint32_t length, uint32_t prot) {
    return _syscall3(SYS_MMAP_ANON, addr, length, prot);
}

/* Change protection flags on an already-mapped range. Returns 0 on
   success, -1 on any page in the range that isn't mapped. */
static inline int32_t sys_mprotect(uint32_t addr, uint32_t length, uint32_t prot) {
    return _syscall3(SYS_MPROTECT, addr, length, prot);
}

#endif
