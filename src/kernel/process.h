#ifndef PROCESS_H
#define PROCESS_H

#include "include/types.h"
#include "kernel/task.h"
#include "kernel/isr.h"
#include "fs/vfs.h"

#define PROCESS_MAX      8
#define FD_MAX           16
#define PROCESS_NAME_MAX 32
#define NSIG             32

#define SIG_INT    2
#define SIG_KILL   9
#define SIG_ALRM  14
#define SIG_TERM  15
#define SIG_CONT  18
#define SIG_STOP  19

typedef enum {
    FD_NONE,
    FD_CONSOLE,
    FD_FILE,
    FD_PIPE_READ,
    FD_PIPE_WRITE,
} fd_type_t;

typedef struct {
    fd_type_t type;
    char      path[VFS_MAX_PATH];
    uint64_t  offset;
    uint32_t  flags;
    void     *pipe;            /* non-NULL for FD_PIPE_{READ,WRITE} */
} fd_entry_t;

#define O_RDONLY 0x00
#define O_WRONLY 0x01
#define O_RDWR   0x02
#define O_CREAT  0x40
#define O_TRUNC  0x200
#define O_APPEND 0x400

#define SPAWN_ARGV_MAX 16
#define SPAWN_ARGV_BUF 512

typedef struct process {
    uint32_t    pid;
    uint32_t    parent_pid;
    uint32_t    pgid;           /* process group — defaults to pid */
    char        name[PROCESS_NAME_MAX];
    task_t     *task;
    fd_entry_t  fds[FD_MAX];
    int         exit_code;
    bool        alive;
    bool        reaped;
    /* Chroot root (host-absolute; "/" = no chroot) and cwd
       (chroot-local, starts at "/"). process_resolve_path
       prepends root before handing a path to the VFS. */
    char        root[VFS_MAX_PATH];
    char        cwd[VFS_MAX_PATH];
    /* Signal state. sig_handlers[0] = SIG_DFL, values 1..NSIG-1
       store user-space handler address (0 = default, 1 = ignore).
       sig_pending is a bitmap of queued signals; sig_delivering
       is set while a handler frame is live on the user stack —
       SYS_SIGRETURN clears it. sig_saved_regs preserves the
       interrupted frame for sigreturn. */
    uint64_t    sig_handlers[NSIG];
    uint32_t    sig_pending;
    bool        sig_delivering;
    registers_t sig_saved_regs;
    /* alarm_ticks: 100 Hz countdown set by SYS_ALARM. Timer IRQ
       decrements; reaches 0 queues SIG_ALRM. */
    uint32_t    alarm_ticks;
    /* Spawn trampoline metadata, populated by process_spawn/execve
       and consumed on first schedule. Per-process so concurrent
       spawns don't race. */
    uint64_t    spawn_entry;
    uint64_t    spawn_rsp;
    int         spawn_argc;
    uint64_t    spawn_argv_off[SPAWN_ARGV_MAX];
    char        spawn_argv_buf[SPAWN_ARGV_BUF];
} process_t;

void       process_init(void);
process_t *process_current(void);
process_t *process_get(uint32_t pid);
process_t *process_alloc(const char *name);
int        process_spawn_from_memory(const char *name, const void *elf,
                                     uint64_t size, char *const argv[]);
/* Read the ELF from `path` via the VFS into a heap buffer, then
   spawn it. Returns the new pid or -1 on error. */
int        process_spawn_from_path(const char *path, char *const argv[]);

/* fd helpers — called from syscall.c. */
int     fd_open(const char *path, uint32_t flags);
int     fd_close(int fd);
int     fd_dup2(int oldfd, int newfd);
int     fd_pipe(int fds[2]);

/* Signal plumbing. */
void     process_kill_foreground(void);
void     process_stop_foreground(void);
int64_t process_signal(int signo, uint64_t handler);
int     process_kill(int32_t pid, int signo);   /* pid<0 kills pgid=-pid */
void    process_tick_alarms(void);
uint32_t process_set_alarm(uint32_t secs);
void    process_sigreturn(registers_t *regs);
void    process_deliver_pending_signals(registers_t *regs);

/* Process-group / foreground tracking. setpgid(pid,pgid): 0 for
   either means "caller's pid". getpgid(pid): 0 means caller. */
int      process_setpgid(uint32_t pid, uint32_t pgid);
uint32_t process_getpgid(uint32_t pid);
uint32_t process_get_foreground(void);
void     process_set_foreground(uint32_t pgid);
ssize_t fd_read(int fd, void *buf, size_t n);
ssize_t fd_write(int fd, const void *buf, size_t n);
off_t   fd_lseek(int fd, off_t off, int whence);
int        process_fork(registers_t *parent_regs);

/* Resolve `path` against the current process's cwd+chroot into
   `out`. Returns 0 on success, -1 if the result overflows. */
int        process_resolve_path(const char *path, char *out, int cap);
/* Replace the current process image with `elf` of `size` bytes.
   Unmaps all user mappings in the current PML4, loads the new
   binary, builds a fresh user stack + argv, and rewrites `regs`
   so the syscall-return iretq lands at the new entry. Returns 0
   on success (caller will iretq into new image), -1 on failure
   (caller keeps running in old image with errno-style return). */
int        process_execve_from_memory(registers_t *regs, const char *name,
                                      const void *elf, uint64_t size,
                                      char *const argv[],
                                      char *const envp[]);
int        process_waitpid(uint32_t pid, int *status);
void       process_exit(int code);
void       process_list_all(void);

/* Snapshot struct for SYS_PS. Layout is part of the user ABI. */
struct proc_info {
    uint32_t pid;
    uint32_t parent_pid;
    uint32_t pgid;
    uint32_t state;         /* matches task_state_t: 0=READY..4=DEAD */
    uint32_t alive;
    char     name[PROCESS_NAME_MAX];
    char     root[VFS_MAX_PATH];
};
/* Fill `out` with info about the idx'th live process (0-based over
   the dense live set). Returns 0 on success, -1 when idx is past
   the end. */
int        process_info_at(uint32_t idx, struct proc_info *out);

#endif
