#ifndef TASK_H
#define TASK_H

#include "include/types.h"
#include "kernel/isr.h"

#define TASK_MAX        16
#define TASK_STACK_SIZE 8192

typedef enum {
    TASK_READY,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_STOPPED,   /* Paused by SIGSTOP; scheduler skips until SIGCONT */
    TASK_DEAD
} task_state_t;

typedef struct task {
    uint32_t      id;
    char          name[32];
    task_state_t  state;
    uint32_t      esp;          /* saved stack pointer (points to registers_t frame) */
    uint32_t      stack_base;   /* bottom of allocated stack (for freeing) */
    struct task  *next;         /* circular linked list for scheduler */
    uint32_t     *pd;           /* page directory (virt == phys, identity-mapped) */
} task_t;

void    task_init(void);
task_t *task_create(const char *name, void (*entry)(void));

/* Split of task_create so fork() can allocate a task without the
   usual entry-point iret frame. task_alloc returns a TASK_READY task
   with a stack but unset esp; task_set_entry lays down the iret frame
   that targets `entry`. task_insert_ready links it into the scheduler
   ring. */
task_t *task_alloc(const char *name);
void    task_set_entry(task_t *t, void (*entry)(void));
void    task_insert_ready(task_t *t);

void    task_yield(void);
void    task_exit(void);
task_t *task_current(void);
uint32_t task_count(void);

/* Helper so mm/vmm.c can resolve the default PD without pulling in
   process state. Returns NULL if scheduling isn't up yet. */
uint32_t *task_current_pd(void);

void task_enable_scheduling(void);
void task_list_all(void);  /* print all tasks to kprintf */

/* Called from timer interrupt — returns new stack pointer */
registers_t *schedule(registers_t *regs);

#endif
