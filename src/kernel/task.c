#include "kernel/task.h"
#include "kernel/tss.h"
#include "kernel/panic.h"
#include "kernel/debug.h"
#include "mm/heap.h"
#include "mm/vmm.h"
#include "lib/string.h"
#include "lib/kprintf.h"

static task_t tasks[TASK_MAX];
static task_t *current;
static task_t *ready_head;  /* head of circular ready list */
static uint32_t next_id = 0;
static bool scheduling_enabled = false;

task_t *task_current(void) {
    return current;
}

uint32_t *task_current_pd(void) {
    return current ? current->pd : 0;
}

uint32_t task_count(void) {
    uint32_t count = 0;
    for (int i = 0; i < TASK_MAX; i++) {
        if (tasks[i].state == TASK_READY || tasks[i].state == TASK_RUNNING ||
            tasks[i].state == TASK_BLOCKED)
            count++;
    }
    return count;
}

static registers_t *yield_handler(registers_t *regs) {
    return schedule(regs);
}

void task_init(void) {
    memset(tasks, 0, sizeof(tasks));

    /* Task 0 = the currently running kernel context (becomes idle task).
       It shares the kernel page directory. */
    task_t *t = &tasks[0];
    t->id = next_id++;
    strncpy(t->name, "idle", sizeof(t->name));
    t->state = TASK_RUNNING;
    t->esp = 0;  /* will be saved on first context switch */
    t->stack_base = 0;  /* uses the original boot stack */
    t->next = t;  /* circular: points to itself */
    t->pd = vmm_kernel_pd();

    current = t;
    ready_head = t;

    isr_register_handler(130, yield_handler);  /* INT 0x82 = yield */
    serial_printf("[task] Initialized, task 0 = idle\n");
}

/* Remove a task from the circular ready list */
static void list_remove(task_t *victim) {
    /* Find the predecessor */
    task_t *prev = victim;
    while (prev->next != victim) {
        prev = prev->next;
        if (prev == victim) return;  /* only element or not in list */
    }
    prev->next = victim->next;
    victim->next = NULL;
}

task_t *task_alloc(const char *name) {
    /* Find a free slot (prefer unused slots, then reuse dead ones) */
    task_t *t = NULL;
    for (int i = 1; i < TASK_MAX; i++) {
        if (tasks[i].name[0] == '\0' && tasks[i].state == 0) {
            t = &tasks[i];
            break;
        }
    }
    if (!t) {
        for (int i = 1; i < TASK_MAX; i++) {
            if (tasks[i].state == TASK_DEAD) {
                /* Remove from circular list before reuse */
                list_remove(&tasks[i]);
                if (tasks[i].stack_base) kfree((void *)tasks[i].stack_base);
                /* Reclaim the dead task's user address space. Safe now
                   because the task is no longer scheduled — we can free
                   the PD without yanking CR3 out from under anyone. */
                if (tasks[i].pd && tasks[i].pd != vmm_kernel_pd()) {
                    vmm_free_pd(tasks[i].pd);
                    tasks[i].pd = 0;
                }
                t = &tasks[i];
                break;
            }
        }
    }
    if (!t) {
        dlog("task_alloc: no free slots\n");
        return NULL;
    }

    memset(t, 0, sizeof(task_t));
    t->id = next_id++;
    strncpy(t->name, name, sizeof(t->name) - 1);
    t->state = TASK_READY;
    /* Inherit the creator's page directory by default. Callers that
       need a fresh address space (process_spawn, fork) overwrite this. */
    t->pd = current ? current->pd : vmm_kernel_pd();

    /* Allocate a kernel stack */
    t->stack_base = (uint32_t)kmalloc(TASK_STACK_SIZE);
    if (!t->stack_base) {
        kprintf("task_alloc: failed to allocate stack\n");
        return NULL;
    }
    memset((void *)t->stack_base, 0, TASK_STACK_SIZE);

    /* esp is left unset — caller must populate it via task_set_entry
       or by synthesizing a registers_t frame (fork). */
    t->esp = 0;
    return t;
}

void task_set_entry(task_t *t, void (*entry)(void)) {
    uint32_t stack_top = t->stack_base + TASK_STACK_SIZE;

    /*
     * Build a fake registers_t frame on the new stack so that when
     * the scheduler switches to this task, isr_common's popa/iret
     * sequence will "return" into the entry function.
     *
     * Ring 0 → ring 0 iret only pops eip/cs/eflags, so we omit ss/useresp.
     */
    uint32_t *sp = (uint32_t *)stack_top;

    *(--sp) = 0x202;           /* eflags: IF set */
    *(--sp) = 0x08;            /* cs: kernel code segment */
    *(--sp) = (uint32_t)entry; /* eip: task entry point */

    *(--sp) = 0;               /* err_code */
    *(--sp) = 0;               /* int_no */

    *(--sp) = 0;  /* eax */
    *(--sp) = 0;  /* ecx */
    *(--sp) = 0;  /* edx */
    *(--sp) = 0;  /* ebx */
    *(--sp) = 0;  /* esp (ignored by popa) */
    *(--sp) = 0;  /* ebp */
    *(--sp) = 0;  /* esi */
    *(--sp) = 0;  /* edi */

    *(--sp) = 0x10;  /* ds */
    *(--sp) = 0x10;  /* es */
    *(--sp) = 0x10;  /* fs */
    *(--sp) = 0x10;  /* gs */

    t->esp = (uint32_t)sp;
}

void task_insert_ready(task_t *t) {
    t->next = current->next;
    current->next = t;
}

task_t *task_create(const char *name, void (*entry)(void)) {
    task_t *t = task_alloc(name);
    if (!t) return NULL;
    task_set_entry(t, entry);
    task_insert_ready(t);
    dlog("[task] Created task %u: %s\n", t->id, t->name);
    return t;
}

static const char *state_names[] = {"READY", "RUNNING", "BLOCKED", "DEAD"};

void task_list_all(void) {
    kprintf("PID  STATE    NAME\n");
    kprintf("---  -------  ----\n");
    for (int i = 0; i < TASK_MAX; i++) {
        task_t *t = &tasks[i];
        if (t->name[0] == '\0' && t->state == 0) continue;
        if (t->state == TASK_DEAD) continue;
        const char *st = (t->state <= TASK_DEAD) ? state_names[t->state] : "???";
        kprintf("%u\t%s\t%s\n", t->id, st, t->name);
    }
}

void task_yield(void) {
    __asm__ volatile ("int $0x82");  /* software yield interrupt (not a hardware IRQ, no EOI) */
}

void task_exit(void) {
    __asm__ volatile ("cli");
    current->state = TASK_DEAD;
    dlog("[task] Task %u (%s) exited\n", current->id, current->name);
    __asm__ volatile ("sti");
    task_yield();
    /* Should not return */
    for (;;) __asm__ volatile ("hlt");
}

void task_enable_scheduling(void) {
    scheduling_enabled = true;
}

registers_t *schedule(registers_t *regs) {
    if (!scheduling_enabled) return regs;
    if (!current) return regs;

    /* Save current task's stack pointer */
    current->esp = (uint32_t)regs;

    /* Find next READY task in the circular list */
    task_t *next = current->next;
    task_t *start = next;
    do {
        if (next->state == TASK_READY || next->state == TASK_RUNNING) {
            break;
        }
        next = next->next;
    } while (next != start);

    /* If no other task is ready, stay on current (or if current is dead, we have a problem) */
    if (next == current && current->state == TASK_DEAD) {
        /* All tasks dead — shouldn't happen if idle task exists */
        panic("No runnable tasks");
    }

    if (next != current) {
        current->state = (current->state == TASK_RUNNING) ? TASK_READY : current->state;
        current = next;
        current->state = TASK_RUNNING;

        /* Update TSS kernel stack so ring 3 → ring 0 transitions
           land on this task's kernel stack top */
        if (current->stack_base) {
            tss_set_kernel_stack(current->stack_base + TASK_STACK_SIZE);
        }

        /* Activate the new task's address space. vmm_switch_pd no-ops
           when the target PD matches the current CR3, so kernel-to-
           kernel switches don't pay for a CR3 reload. */
        if (current->pd) {
            vmm_switch_pd(current->pd);
        }
    }

    return (registers_t *)current->esp;
}
