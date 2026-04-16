#ifndef PROCESS_H
#define PROCESS_H

#include "include/types.h"
#include "kernel/task.h"
#include "kernel/isr.h"
#include "fs/vfs.h"

#define PROCESS_MAX      16
#define FD_MAX           16
#define PROCESS_NAME_MAX 32
#define NSIG             32

typedef enum {
    FD_NONE,
    FD_CONSOLE,
    FD_FILE,
    FD_PIPE_READ,
    FD_PIPE_WRITE
} fd_type_t;

/* `pipe` is non-NULL iff type is FD_PIPE_READ / FD_PIPE_WRITE. Stored
   as void* to keep the pipe_t detail out of this header. */
typedef struct {
    fd_type_t type;
    char      path[VFS_MAX_PATH];
    uint32_t  offset;
    uint32_t  flags;
    void     *pipe;
} fd_entry_t;

#define O_RDONLY  0x00
#define O_WRONLY  0x01
#define O_RDWR    0x02
#define O_CREAT   0x40
#define O_TRUNC   0x200
#define O_APPEND  0x400

#define SPAWN_ARGV_MAX 16
#define SPAWN_ARGV_BUF 512
#define SPAWN_ENVP_MAX 32
#define SPAWN_ENVP_BUF 1024

typedef struct process {
    uint32_t     pid;
    uint32_t     parent_pid;
    uint32_t     pgid;        /* process group ID; default = pid */
    char         name[PROCESS_NAME_MAX];
    task_t      *task;
    fd_entry_t   fds[FD_MAX];
    int          exit_code;
    bool         alive;
    uint32_t     brk;
    char         cwd[VFS_MAX_PATH];
    /* Chroot directory, host-absolute. "/" or empty means no chroot.
       All process_resolve_path output is prepended with this before
       being handed to the VFS. Inherited through fork/spawn, preserved
       across execve. `..` at cwd='/' clamps at the chroot boundary via
       canon_into's existing clamp. */
    char         root[VFS_MAX_PATH];
    /* Pending spawn metadata — populated during sys_spawn / sys_execve,
       consumed by the child's trampoline on first schedule. Kept per-
       process so concurrent spawns (shell pipelines) don't clobber each
       other via shared globals. */
    uint32_t     spawn_entry_point;
    uint32_t     spawn_stack_top;
    int          spawn_argc;
    uint32_t     spawn_argv_off[SPAWN_ARGV_MAX];
    char         spawn_argv_buf[SPAWN_ARGV_BUF];
    /* Environment passed via SYS_SPAWN/SYS_EXECVE. Snapshotted like
       argv so back-to-back spawns don't race. When the caller passes
       envp=NULL the kernel inherits the current process's environ
       snapshot. */
    int          spawn_envc;
    uint32_t     spawn_envp_off[SPAWN_ENVP_MAX];
    char         spawn_envp_buf[SPAWN_ENVP_BUF];
    /* User-space signal handlers. Entry value: 0 = SIG_DFL (default
       action — terminate on SIGINT, ignore on SIGCHLD-style signals
       we don't yet model), 1 = SIG_IGN (silently drop), anything else
       = user-space function address invoked with signo as its argument.
       SIG_KILL / SIG_STOP are never catchable — SYS_SIGNAL rejects them.
       sig_pending bitmap holds queued signals awaiting delivery;
       sig_delivering guards against re-entrant delivery while a handler
       is still running; sig_saved_regs snapshots the interrupted
       user frame so SYS_SIGRETURN can restore it verbatim. */
    uint32_t     sig_handlers[NSIG];
    uint32_t     sig_pending;
    bool         sig_delivering;
    registers_t  sig_saved_regs;
    /* Countdown for SYS_ALARM in 100Hz timer ticks. Decremented from
       the timer IRQ; reaches 0 → queues SIG_ALRM and stays at 0. */
    uint32_t     alarm_ticks;
    /* Dynamic-linking metadata for a main executable that ships with
       PT_INTERP. Zero in all fields means "static binary — no interp".
       Populated in process_spawn/execve *before* the trampoline runs;
       consumed by setup_user_stack to emit the SysV aux vector so the
       interpreter (ld-lighthos.so.1) can find main's phdrs + entry. */
    uint32_t     main_phdr_vaddr;   /* AT_PHDR */
    uint32_t     main_phnum;        /* AT_PHNUM */
    uint32_t     main_entry;        /* AT_ENTRY */
    uint32_t     interp_base;       /* AT_BASE */
    uint32_t     interp_entry;      /* first user EIP (replaces main_entry) */
} process_t;

/* Compact snapshot of a process, suitable for user-space consumption. */
typedef struct {
    uint32_t pid;
    uint32_t parent_pid;
    uint32_t pgid;
    uint32_t state;   /* 0=ready, 1=running, 2=blocked, 3=stopped, 4=dead */
    char     name[PROCESS_NAME_MAX];
    char     root[VFS_MAX_PATH];
} proc_info_t;

void       process_init(void);
process_t *process_create(const char *name, void (*entry)(void));

/* Foreground-pgid tracking: the shell claims foreground, hands it off
   to children during waitpid, and reclaims on return. Used by the
   serial driver to route Ctrl-C / Ctrl-Z to all processes in the
   foreground group. */
void       process_set_foreground(uint32_t pgid);
uint32_t   process_get_foreground(void);
/* Deliver SIGINT (Ctrl-C) to the foreground group — terminate every
   live process whose pgid matches foreground_pgid. */
void       process_kill_foreground(void);
/* Deliver SIGSTOP (Ctrl-Z) to the foreground group — move every
   matching task to TASK_STOPPED. Also unblocks the parent waitpid
   so the shell returns to its prompt. */
void       process_stop_foreground(void);
/* Signal numbers (Linux-compatible subset). */
#define SIG_HUP   1
#define SIG_INT   2
#define SIG_KILL  9
#define SIG_ALRM 14
#define SIG_TERM 15
#define SIG_CONT 18
#define SIG_STOP 19
/* Deliver `signo` to every process whose pid == target (target > 0)
   or whose pgid == -target (target < 0). Returns 0 on success, -1 if
   no process matched. */
int        process_signal(int target, int signo);

/* Install a user-space signal handler for `signo`. Returns previous
   handler address (0 = SIG_DFL, 1 = SIG_IGN). SIG_KILL and SIG_STOP
   are uncatchable and return -1. */
int32_t    process_sig_install(int signo, uint32_t handler);

/* Return-from-handler path: restores regs from sig_saved_regs and
   clears the delivering flag. Called from SYS_SIGRETURN. */
void       process_sigreturn(registers_t *regs);

/* Called at the tail of syscall_handler and schedule() before iret
   back to user mode. If the current process has pending signals with
   user handlers and isn't already in one, rewrites regs to trampoline
   into the handler — saving the original regs in sig_saved_regs and
   pushing signo onto the user stack. No-op when regs targets ring 0. */
void       process_deliver_pending_signals(registers_t *regs);

/* Called from the 100Hz timer IRQ. Decrements every live process's
   alarm_ticks; when a counter hits 0, queues SIG_ALRM on that process.
   IRQ-safe (assumes caller disabled interrupts or is in IRQ context).
   Sets `secs` seconds as the new alarm (0 cancels); returns seconds
   remaining from the previous alarm. */
void       process_tick_alarms(void);
uint32_t   process_set_alarm(uint32_t secs);

/* Resolve a user-supplied path against the current process's cwd and
   canonicalize `.` / `..` segments. `out` receives the absolute
   result; `out_size` is its capacity. Returns 0 on success, -1 if
   the result wouldn't fit. Used by every path-taking syscall so
   user programs can use relative paths without resolving themselves. */
int        process_resolve_path(const char *path, char *out, int out_size);
/* Strip a process's chroot `root` from a host-absolute path, in-place.
   Used to convert resolved paths back to chroot-local form for
   storage (e.g. cwd), so subsequent resolve calls don't double-prepend
   the root. */
void       process_strip_root_prefix(char *buf, const char *root);
/* Allocate a process slot without creating a task. Used by fork so the
   caller can attach its own task_alloc'd task with a custom stack. */
process_t *process_alloc(const char *name);
int        process_fork(registers_t *parent_regs);
int        process_execve(registers_t *regs, const char *path,
                          char *const argv[], char *const envp[]);
process_t *process_current(void);
process_t *process_get(uint32_t pid);
void       process_exit(int code);
int        process_waitpid(uint32_t pid, int *status);
void       process_list_all(void);
/* Spawn an ELF at `path`. argv is a NULL-terminated array of string
   pointers in caller address space (may be NULL, in which case argc=1
   and argv[0]=path). Strings are snapshotted into kernel memory before
   scheduling so the new task can read them after the parent's memory
   is no longer accessible. */
int        process_spawn(const char *path, char *const argv[],
                         char *const envp[]);
/* Fill `out` with the idx-th live process. Returns 0 on success, -1 when
   idx is past the last live entry. */
int        process_info(uint32_t idx, proc_info_t *out);

/* File descriptor operations */
int     fd_open(const char *path, uint32_t flags);
int     fd_close(int fd);
int     fd_dup2(int oldfd, int newfd);
/* Allocate a read/write pipe pair into the caller's fd table. Returns
   0 on success and fills out_fds[0] (read end) / out_fds[1] (write
   end); -1 on OOM or fd exhaustion. */
int     fd_pipe(int out_fds[2]);
ssize_t fd_read(int fd, void *buf, size_t count);
ssize_t fd_write(int fd, const void *buf, size_t count);
/* Reposition fd. whence: 0=SEEK_SET, 1=SEEK_CUR, 2=SEEK_END.
   Returns the new offset, or (off_t)-1 on error. */
off_t   fd_lseek(int fd, off_t offset, int whence);

#endif
