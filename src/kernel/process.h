#ifndef PROCESS_H
#define PROCESS_H

#include "include/types.h"
#include "kernel/task.h"
#include "fs/vfs.h"

#define PROCESS_MAX      16
#define FD_MAX           16
#define PROCESS_NAME_MAX 32

typedef enum {
    FD_NONE,
    FD_CONSOLE,
    FD_FILE
} fd_type_t;

typedef struct {
    fd_type_t type;
    char      path[VFS_MAX_PATH];
    uint32_t  offset;
    uint32_t  flags;
} fd_entry_t;

#define O_RDONLY  0x00
#define O_WRONLY  0x01
#define O_RDWR    0x02
#define O_CREAT   0x40
#define O_TRUNC   0x200
#define O_APPEND  0x400

typedef struct process {
    uint32_t     pid;
    uint32_t     parent_pid;
    char         name[PROCESS_NAME_MAX];
    task_t      *task;
    fd_entry_t   fds[FD_MAX];
    int          exit_code;
    bool         alive;
    uint32_t     brk;
    char         cwd[VFS_MAX_PATH];
} process_t;

/* Compact snapshot of a process, suitable for user-space consumption. */
typedef struct {
    uint32_t pid;
    uint32_t parent_pid;
    uint32_t state;   /* 0=ready, 1=running, 2=blocked, 3=dead */
    char     name[PROCESS_NAME_MAX];
} proc_info_t;

void       process_init(void);
process_t *process_create(const char *name, void (*entry)(void));

/* Foreground-pid tracking: the shell claims foreground, hands it off
   to children during waitpid, and reclaims on return. Used by the
   serial driver to route Ctrl-C to the right process. */
void       process_set_foreground(uint32_t pid);
uint32_t   process_get_foreground(void);
/* Terminate the current foreground process (Ctrl-C path). No-op if
   foreground is pid 0 / the shell itself. Called from interrupt
   context in the serial callback. */
void       process_kill_foreground(void);

/* Resolve a user-supplied path against the current process's cwd and
   canonicalize `.` / `..` segments. `out` receives the absolute
   result; `out_size` is its capacity. Returns 0 on success, -1 if
   the result wouldn't fit. Used by every path-taking syscall so
   user programs can use relative paths without resolving themselves. */
int        process_resolve_path(const char *path, char *out, int out_size);
/* Allocate a process slot without creating a task. Used by fork so the
   caller can attach its own task_alloc'd task with a custom stack. */
process_t *process_alloc(const char *name);
int        process_fork(registers_t *parent_regs);
int        process_execve(registers_t *regs, const char *path, char *const argv[]);
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
int        process_spawn(const char *path, char *const argv[]);
/* Fill `out` with the idx-th live process. Returns 0 on success, -1 when
   idx is past the last live entry. */
int        process_info(uint32_t idx, proc_info_t *out);

/* File descriptor operations */
int     fd_open(const char *path, uint32_t flags);
int     fd_close(int fd);
int     fd_dup2(int oldfd, int newfd);
ssize_t fd_read(int fd, void *buf, size_t count);
ssize_t fd_write(int fd, const void *buf, size_t count);
/* Reposition fd. whence: 0=SEEK_SET, 1=SEEK_CUR, 2=SEEK_END.
   Returns the new offset, or (off_t)-1 on error. */
off_t   fd_lseek(int fd, off_t offset, int whence);

#endif
