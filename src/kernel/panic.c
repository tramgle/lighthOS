#include "kernel/panic.h"
#include "lib/kprintf.h"

void panic(const char *msg) {
    __asm__ volatile ("cli");
    kprintf("\n*** KERNEL PANIC: %s ***\n", msg);
    kprintf("System halted.\n");
    for (;;) {
        __asm__ volatile ("hlt");
    }
}
