#ifndef TASK_H
#define TASK_H

#include "include/types.h"
#include "kernel/isr.h"

#define TASK_MAX        64
#define TASK_STACK_SIZE 16384

typedef enum {
    TASK_READY,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_STOPPED,
    TASK_DEAD
} task_state_t;

typedef struct task {
    uint32_t      id;
    char          name[32];
    task_state_t  state;
    uint64_t      rsp;            /* saved stack pointer (→ registers_t frame) */
    uint64_t      stack_base;     /* kmalloc'd base (for free) */
    struct task  *next;
    uint64_t     *pml4;           /* per-process PML4 (virt via identity map) */
    /* FPU/SSE state — FXSAVE emits 512 bytes, 16-byte aligned.
       Initialized from a clean default on task_alloc; swapped in
       schedule() so user-space XMM usage doesn't leak across
       tasks. */
    uint8_t       fxstate[512] __attribute__((aligned(16)));
} task_t;

void    task_init(void);
task_t *task_create(const char *name, void (*entry)(void));
task_t *task_alloc(const char *name);
void    task_set_entry(task_t *t, void (*entry)(void));
void    task_insert_ready(task_t *t);
void    task_yield(void);
void    task_exit(void);
task_t *task_current(void);
uint32_t task_count(void);
uint64_t *task_current_pml4(void);
void    task_enable_scheduling(void);
void    task_list_all(void);

registers_t *schedule(registers_t *regs);

#endif
