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

#define O_RDONLY 0x00
#define O_WRONLY 0x01
#define O_RDWR   0x02
#define O_CREAT  0x40
#define O_TRUNC  0x200
#define O_APPEND 0x400

typedef unsigned int uint32_t;
typedef int int32_t;

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
static inline int32_t sys_spawn(const char *path, char *const argv[]) { return _syscall2(SYS_SPAWN, (uint32_t)path, (uint32_t)argv); }
static inline int32_t sys_waitpid(uint32_t pid, int *status) { return _syscall2(SYS_WAITPID, pid, (uint32_t)status); }
static inline int32_t sys_readdir(const char *path, uint32_t idx, char *name, uint32_t *type) { return _syscall4(SYS_READDIR, (uint32_t)path, idx, (uint32_t)name, (uint32_t)type); }
static inline int32_t sys_chdir(const char *path) { return _syscall1(SYS_CHDIR, (uint32_t)path); }
static inline int32_t sys_getcwd(char *buf, uint32_t size) { return _syscall2(SYS_GETCWD, (uint32_t)buf, size); }
static inline int32_t sys_fork(void) { return _syscall0(SYS_FORK); }
static inline int32_t sys_execve(const char *path, char *const argv[]) { return _syscall2(SYS_EXECVE, (uint32_t)path, (uint32_t)argv); }

#define PROC_NAME_MAX 32
struct proc_info {
    uint32_t pid;
    uint32_t parent_pid;
    uint32_t state;
    char     name[PROC_NAME_MAX];
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

#endif
