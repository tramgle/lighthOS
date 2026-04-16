#include "kernel/panic.h"
#include "kernel/debug.h"
#include "lib/kprintf.h"

void panic(const char *msg) {
    __asm__ volatile ("cli");
    kprintf("\n*** KERNEL PANIC: %s ***\n", msg);
    debug_backtrace((uint64_t)(uintptr_t)__builtin_frame_address(0));
    kprintf("System halted.\n");
    for (;;) __asm__ volatile ("hlt");
}
