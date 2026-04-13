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

void isr_handler(registers_t *regs) {
    if (handlers[regs->int_no]) {
        handlers[regs->int_no](regs);
    } else if (regs->int_no < 32) {
        const char *name = "Unknown Exception";
        if (regs->int_no < sizeof(exception_names) / sizeof(exception_names[0])) {
            name = exception_names[regs->int_no];
        }
        kprintf("\nException: %s (#%u) err=0x%x eip=0x%x\n",
                name, regs->int_no, regs->err_code, regs->eip);
        panic("Unhandled CPU exception");
    }

    /* Send EOI for hardware IRQs */
    if (regs->int_no >= 32 && regs->int_no < 48) {
        pic_send_eoi(regs->int_no - 32);
    }
}
