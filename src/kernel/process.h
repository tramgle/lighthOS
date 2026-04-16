#ifndef PROCESS_H
#define PROCESS_H

#include "include/types.h"
#include "kernel/task.h"
#include "kernel/isr.h"

#define PROCESS_MAX      8
#define FD_MAX           16
#define PROCESS_NAME_MAX 32

typedef enum {
    FD_NONE,
    FD_CONSOLE,
} fd_type_t;

typedef struct {
    fd_type_t type;
} fd_entry_t;

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
int        process_fork(registers_t *parent_regs);
int        process_waitpid(uint32_t pid, int *status);
void       process_exit(int code);
void       process_list_all(void);

#endif
