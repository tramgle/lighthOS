#ifndef PROCESS_H
#define PROCESS_H

#include "include/types.h"
#include "kernel/task.h"
#include "kernel/isr.h"
#include "fs/vfs.h"

#define PROCESS_MAX      8
#define FD_MAX           16
#define PROCESS_NAME_MAX 32

typedef enum {
    FD_NONE,
    FD_CONSOLE,
    FD_FILE,
} fd_type_t;

typedef struct {
    fd_type_t type;
    char      path[VFS_MAX_PATH];
    uint64_t  offset;
    uint32_t  flags;
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
    char        name[PROCESS_NAME_MAX];
    task_t     *task;
    fd_entry_t  fds[FD_MAX];
    int         exit_code;
    bool        alive;
    bool        reaped;
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

/* fd helpers — called from syscall.c. */
int     fd_open(const char *path, uint32_t flags);
int     fd_close(int fd);
ssize_t fd_read(int fd, void *buf, size_t n);
ssize_t fd_write(int fd, const void *buf, size_t n);
off_t   fd_lseek(int fd, off_t off, int whence);
int        process_fork(registers_t *parent_regs);
/* Replace the current process image with `elf` of `size` bytes.
   Unmaps all user mappings in the current PML4, loads the new
   binary, builds a fresh user stack + argv, and rewrites `regs`
   so the syscall-return iretq lands at the new entry. Returns 0
   on success (caller will iretq into new image), -1 on failure
   (caller keeps running in old image with errno-style return). */
int        process_execve_from_memory(registers_t *regs, const char *name,
                                      const void *elf, uint64_t size,
                                      char *const argv[]);
int        process_waitpid(uint32_t pid, int *status);
void       process_exit(int code);
void       process_list_all(void);

#endif
