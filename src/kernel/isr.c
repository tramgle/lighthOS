#include "kernel/isr.h"
#include "kernel/pic.h"
#include "lib/kprintf.h"
#include "kernel/panic.h"

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

    kprintf("\nException: %s (#%lu) err=0x%lx rip=0x%lx\n",
            name, regs->int_no, regs->err_code, regs->rip);

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
}

registers_t *isr_handler(registers_t *regs) {
    registers_t *ret = regs;

    if (handlers[regs->int_no]) {
        ret = handlers[regs->int_no](regs);
    } else if (regs->int_no < 32) {
        dump_exception(regs);
        panic("Unhandled CPU exception");
    }

    if (regs->int_no >= 32 && regs->int_no < 48) {
        pic_send_eoi((uint8_t)(regs->int_no - 32));
    }

    return ret;
}
