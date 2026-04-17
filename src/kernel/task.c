#include "kernel/task.h"
#include "kernel/tss.h"
#include "kernel/panic.h"
#include "kernel/gdt.h"
#include "mm/heap.h"
#include "mm/vmm.h"
#include "lib/string.h"
#include "lib/kprintf.h"

void process_deliver_pending_signals(registers_t *regs);

static task_t tasks[TASK_MAX];
static task_t *current;
static task_t *ready_head;
static uint32_t next_id = 0;
static bool scheduling_enabled = false;

task_t *task_current(void)          { return current; }
uint64_t *task_current_pml4(void)   { return current ? current->pml4 : 0; }

uint32_t task_count(void) {
    uint32_t n = 0;
    for (int i = 0; i < TASK_MAX; i++) {
        task_state_t s = tasks[i].state;
        if (s == TASK_READY || s == TASK_RUNNING ||
            s == TASK_BLOCKED || s == TASK_STOPPED) n++;
    }
    return n;
}

static registers_t *yield_handler(registers_t *regs) { return schedule(regs); }

void task_init(void) {
    memset(tasks, 0, sizeof(tasks));

    task_t *t = &tasks[0];
    t->id = next_id++;
    strncpy(t->name, "idle", sizeof(t->name));
    t->state = TASK_RUNNING;
    t->rsp = 0;
    t->stack_base = 0;
    t->next = t;
    t->pml4 = vmm_kernel_pml4();
    /* Clean default FPU/SSE state for the idle task. */
    __asm__ volatile ("fninit; fxsave (%0)" :: "r"(t->fxstate) : "memory");

    current = t;
    ready_head = t;

    isr_register_handler(0x82, yield_handler);   /* INT 0x82 = yield */
    serial_printf("[task] initialised; task 0 = idle\n");
}

static void list_remove(task_t *victim) {
    task_t *prev = victim;
    while (prev->next != victim) {
        prev = prev->next;
        if (prev == victim) return;
    }
    prev->next = victim->next;
    victim->next = 0;
}

task_t *task_alloc(const char *name) {
    task_t *t = 0;
    for (int i = 1; i < TASK_MAX; i++) {
        if (tasks[i].name[0] == '\0' && tasks[i].state == 0) { t = &tasks[i]; break; }
    }
    if (!t) {
        for (int i = 1; i < TASK_MAX; i++) {
            if (tasks[i].state == TASK_DEAD) {
                list_remove(&tasks[i]);
                if (tasks[i].stack_base) kfree((void *)(uintptr_t)tasks[i].stack_base);
                if (tasks[i].pml4 && tasks[i].pml4 != vmm_kernel_pml4()) {
                    vmm_free_pml4(tasks[i].pml4);
                }
                t = &tasks[i];
                break;
            }
        }
    }
    if (!t) { kprintf("task_alloc: no free slots\n"); return 0; }

    memset(t, 0, sizeof(*t));
    t->id = next_id++;
    strncpy(t->name, name, sizeof(t->name) - 1);
    t->state = TASK_READY;
    t->pml4 = current ? current->pml4 : vmm_kernel_pml4();

    t->stack_base = (uint64_t)(uintptr_t)kmalloc(TASK_STACK_SIZE);
    if (!t->stack_base) { kprintf("task_alloc: stack OOM\n"); return 0; }
    memset((void *)(uintptr_t)t->stack_base, 0, TASK_STACK_SIZE);
    t->rsp = 0;

    /* Default FPU/SSE state. MXCSR=0x1F80 (default masks), FPU
       control word=0x037F, status/tag words zero. The simplest
       way to get a known-good FXSAVE image is to FNINIT + FXSAVE.
       Do it here so new tasks start with a fresh x87/SSE state. */
    {
        uint8_t tmp[512] __attribute__((aligned(16)));
        __asm__ volatile ("fninit; fxsave (%0)" :: "r"(tmp) : "memory");
        for (int i = 0; i < 512; i++) t->fxstate[i] = tmp[i];
    }
    return t;
}

/* Lay down a registers_t on the kernel stack so schedule() + the
   ISR return path iretq into `entry` in kernel CS. Ring 0 → ring 0
   iretq still pops SS/RSP/RFLAGS/CS/RIP in long mode. */
void task_set_entry(task_t *t, void (*entry)(void)) {
    uint64_t stack_top = t->stack_base + TASK_STACK_SIZE;
    uint64_t *sp = (uint64_t *)(uintptr_t)stack_top;

    /* CPU iretq frame (built top-down from high addresses) */
    *(--sp) = GDT_KERNEL_DATA;            /* ss */
    *(--sp) = stack_top;                  /* rsp (restore same stack top) */
    *(--sp) = 0x202;                      /* rflags with IF=1 */
    *(--sp) = GDT_KERNEL_CODE;            /* cs */
    *(--sp) = (uint64_t)(uintptr_t)entry; /* rip */

    /* Stub-pushed err + int_no */
    *(--sp) = 0;                          /* err_code */
    *(--sp) = 0;                          /* int_no */

    /* 15 GPRs saved by isr_common in reverse-pop order */
    *(--sp) = 0;                          /* r15 */
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;
    *(--sp) = 0;                          /* r8 */
    *(--sp) = 0;                          /* rbp */
    *(--sp) = 0;                          /* rdi */
    *(--sp) = 0;                          /* rsi */
    *(--sp) = 0;                          /* rdx */
    *(--sp) = 0;                          /* rcx */
    *(--sp) = 0;                          /* rbx */
    *(--sp) = 0;                          /* rax */

    t->rsp = (uint64_t)(uintptr_t)sp;
}

void task_insert_ready(task_t *t) {
    t->next = current->next;
    current->next = t;
}

task_t *task_create(const char *name, void (*entry)(void)) {
    task_t *t = task_alloc(name);
    if (!t) return 0;
    task_set_entry(t, entry);
    task_insert_ready(t);
    return t;
}

static const char *state_names[] = {"READY","RUNNING","BLOCKED","STOPPED","DEAD"};

void task_list_all(void) {
    kprintf("PID  STATE    NAME\n");
    for (int i = 0; i < TASK_MAX; i++) {
        task_t *t = &tasks[i];
        if (t->name[0] == '\0' && t->state == 0) continue;
        if (t->state == TASK_DEAD) continue;
        const char *st = (t->state <= TASK_DEAD) ? state_names[t->state] : "???";
        kprintf("%u  %s  %s\n", t->id, st, t->name);
    }
}

void task_yield(void) {
    __asm__ volatile ("int $0x82");
}

void task_exit(void) {
    __asm__ volatile ("cli");
    current->state = TASK_DEAD;
    __asm__ volatile ("sti");
    task_yield();
    for (;;) __asm__ volatile ("hlt");
}

void task_enable_scheduling(void) { scheduling_enabled = true; }

registers_t *schedule(registers_t *regs) {
    if (!scheduling_enabled) return regs;
    if (!current) return regs;

    current->rsp = (uint64_t)(uintptr_t)regs;

    task_t *next = current->next;
    task_t *start = next;
    do {
        if (next->state == TASK_READY || next->state == TASK_RUNNING) break;
        next = next->next;
    } while (next != start);

    if (next == current && current->state == TASK_DEAD) panic("No runnable tasks");

    if (next != current) {
        /* Save outgoing task's FPU/SSE state and load incoming's.
           CR4.OSFXSR was set in boot.s so FXSAVE/FXRSTOR are legal. */
        __asm__ volatile ("fxsave (%0)" :: "r"(current->fxstate) : "memory");
        if (current->state == TASK_RUNNING) current->state = TASK_READY;
        task_t *prev = current;
        current = next;
        current->state = TASK_RUNNING;
        __asm__ volatile ("fxrstor (%0)" :: "r"(current->fxstate) : "memory");
        if (current->stack_base) {
            tss_set_kernel_stack(current->stack_base + TASK_STACK_SIZE);
        }
        /* Skip the CR3 write (and its TLB flush) when the incoming
           task shares an address space with the outgoing one. Kernel
           tasks + threads-within-a-process hit this today; the
           bigger win is that fork + execve intentionally give each
           process its own PML4, so the common case is a full flush —
           but an (admittedly minor) pgroup-wide fanout still benefits. */
        if (current->pml4 && current->pml4 != prev->pml4)
            vmm_switch_pml4(current->pml4);
    }

    registers_t *ret = (registers_t *)(uintptr_t)current->rsp;
    process_deliver_pending_signals(ret);
    return ret;
}
