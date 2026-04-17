#include "kernel/isr.h"
#include "kernel/pic.h"
#include "kernel/debug.h"
#include "kernel/ksyms.h"
#include "kernel/task.h"
#include "kernel/process.h"
#include "lib/kprintf.h"
#include "kernel/panic.h"

extern void process_deliver_pending_signals(registers_t *regs);

static isr_handler_t handlers[256];

static const char *exception_names[] = {
    "Division By Zero",
    "Debug",
    "Non Maskable Interrupt",
    "Breakpoint",
    "Overflow",
    "Bound Range Exceeded",
    "Invalid Opcode",
    "Device Not Available",
    "Double Fault",
    "Coprocessor Segment Overrun",
    "Invalid TSS",
    "Segment Not Present",
    "Stack-Segment Fault",
    "General Protection Fault",
    "Page Fault",
    "Reserved",
    "x87 FPU Error",
    "Alignment Check",
    "Machine Check",
    "SIMD Floating-Point",
    "Virtualization Exception",
    "Control Protection Exception",
};

void isr_register_handler(uint8_t n, isr_handler_t handler) {
    handlers[n] = handler;
}

static void dump_exception(registers_t *regs) {
    const char *name = "Unknown Exception";
    if (regs->int_no < sizeof(exception_names) / sizeof(exception_names[0])) {
        name = exception_names[regs->int_no];
    }
    bool from_user = (regs->cs & 3) == 3;

    uint64_t rip_off = 0;
    const char *rip_sym = ksym_lookup(regs->rip, &rip_off);
    kprintf("\nException: %s (#%lu) err=0x%lx rip=0x%lx", name, regs->int_no,
            regs->err_code, regs->rip);
    if (rip_sym) kprintf("  %s+0x%lx", rip_sym, rip_off);
    kprintf("\n");

    if (regs->int_no == 14) {
        uint64_t cr2;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
        kprintf("  CR2=0x%lx  (P=%u W=%u U=%u)\n", cr2,
                (uint32_t)((regs->err_code >> 0) & 1),
                (uint32_t)((regs->err_code >> 1) & 1),
                (uint32_t)((regs->err_code >> 2) & 1));
    }

    kprintf("  rax=0x%lx rbx=0x%lx rcx=0x%lx rdx=0x%lx\n",
            regs->rax, regs->rbx, regs->rcx, regs->rdx);
    kprintf("  rsi=0x%lx rdi=0x%lx rbp=0x%lx rsp=0x%lx\n",
            regs->rsi, regs->rdi, regs->rbp, regs->rsp);
    kprintf("  r8 =0x%lx r9 =0x%lx r10=0x%lx r11=0x%lx\n",
            regs->r8, regs->r9, regs->r10, regs->r11);
    kprintf("  r12=0x%lx r13=0x%lx r14=0x%lx r15=0x%lx\n",
            regs->r12, regs->r13, regs->r14, regs->r15);
    kprintf("  cs=0x%lx ss=0x%lx rflags=0x%lx  (%s)\n",
            regs->cs, regs->ss, regs->rflags,
            from_user ? "ring 3" : "ring 0");

    /* Only unwind frame pointers for ring-0 exceptions. Walking a
       user RBP chain from kernel mode risks faulting on unmapped
       user memory, which would re-enter this path and loop. */
    if (!from_user) debug_backtrace(regs->rbp);
}

/* Ring-3 synchronous CPU exceptions (#0, #6, #13, #14) terminate the
   faulting process instead of panicking the kernel. We DON'T route
   this through the signal machinery: the faulting instruction hasn't
   retired, so just marking the signal pending and iretq'ing would
   re-execute the same instruction and refault forever. Instead we
   mark the task DEAD in place and hand the scheduler a different
   task's registers_t to return to, so iretq lands in that task's
   user frame and the dead one never runs again.

   Kernel-mode faults still panic — they indicate a real bug in the
   kernel path that produced them. */
static registers_t *user_fault_terminate(registers_t *regs, int signo) {
    process_t *p = process_current();

    uint64_t cr2 = 0;
    if (regs->int_no == 14) __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));

    uint64_t off = 0;
    const char *sym = ksym_lookup(regs->rip, &off);
    serial_printf("[fault] pid=%u name=%s sig=%d int=%lu err=0x%lx rip=0x%lx%s%s%s cr2=0x%lx\n",
                  p ? p->pid : 0, p ? p->name : "?", signo,
                  regs->int_no, regs->err_code, regs->rip,
                  sym ? " (" : "", sym ? sym : "", sym ? ")" : "",
                  cr2);

    if (p) {
        p->exit_code = 128 + signo;
        p->alive = false;
        if (p->task) p->task->state = TASK_DEAD;
    }

    /* Hand the scheduler a fresh frame. Without this the CPU would
       iretq back to the faulting rip and trap again immediately. */
    return schedule(regs);
}

registers_t *isr_handler(registers_t *regs) {
    registers_t *ret = regs;

    if (handlers[regs->int_no]) {
        ret = handlers[regs->int_no](regs);
    } else if (regs->int_no < 32) {
        bool from_user = (regs->cs & 3) == 3;
        if (from_user) {
            /* SIGSEGV for memory faults, SIGILL for invalid opcode,
               SIGFPE for divide/FPU — all default-terminate. */
            int signo = 11;   /* SIGSEGV */
            if (regs->int_no == 0)      signo = 8;   /* SIGFPE (div0) */
            else if (regs->int_no == 6) signo = 4;   /* SIGILL */
            else if (regs->int_no == 16 ||
                     regs->int_no == 19) signo = 8;  /* SIGFPE (FPU/SSE) */
            dump_exception(regs);
            ret = user_fault_terminate(regs, signo);
        } else {
            dump_exception(regs);
            panic("Unhandled CPU exception");
        }
    }

    if (regs->int_no >= 32 && regs->int_no < 48) {
        pic_send_eoi((uint8_t)(regs->int_no - 32));
    }

    /* Deliver any pending user-space signal. Runs for every
       interrupt return, so a SIGALRM queued by the timer IRQ or
       a SIGINT queued by SYS_KILL is picked up on the next hop
       back to ring 3. No-op for kernel-mode frames. */
    process_deliver_pending_signals(ret);

    return ret;
}
