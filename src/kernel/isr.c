#include "kernel/isr.h"
#include "kernel/pic.h"
#include "kernel/debug.h"
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

    kprintf("\nException: %s (#%u) err=0x%x eip=0x%x\n",
            name, regs->int_no, regs->err_code, regs->eip);

    if (regs->int_no == 14) {
        uint32_t cr2;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
        kprintf("  CR2=0x%x  (P=%u W=%u U=%u)\n",
                cr2,
                (regs->err_code >> 0) & 1,
                (regs->err_code >> 1) & 1,
                (regs->err_code >> 2) & 1);
    }

    kprintf("  eax=0x%x ebx=0x%x ecx=0x%x edx=0x%x\n",
            regs->eax, regs->ebx, regs->ecx, regs->edx);
    kprintf("  esi=0x%x edi=0x%x ebp=0x%x esp=0x%x\n",
            regs->esi, regs->edi, regs->ebp, regs->esp);
    kprintf("  cs=0x%x ds=0x%x eflags=0x%x\n",
            regs->cs, regs->ds, regs->eflags);

    if (from_user) {
        kprintf("  ss=0x%x useresp=0x%x  (ring 3)\n", regs->ss, regs->useresp);
    } else {
        debug_backtrace(regs->ebp);
    }
}

registers_t *isr_handler(registers_t *regs) {
    registers_t *ret = regs;

    if (handlers[regs->int_no]) {
        ret = handlers[regs->int_no](regs);
    } else if (regs->int_no < 32) {
        dump_exception(regs);
        panic("Unhandled CPU exception");
    }

    /* Send EOI for hardware IRQs */
    if (regs->int_no >= 32 && regs->int_no < 48) {
        pic_send_eoi(regs->int_no - 32);
    }

    return ret;
}
