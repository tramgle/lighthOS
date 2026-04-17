#include "kernel/panic.h"
#include "kernel/debug.h"
#include "lib/kprintf.h"

void panic(const char *msg) {
    __asm__ volatile ("cli");
    kprintf("\n*** KERNEL PANIC: %s ***\n", msg);
    debug_backtrace((uint64_t)(uintptr_t)__builtin_frame_address(0));
    /* Persist the whole boot-log ring — ending with the panic msg
       and backtrace we just printed — so a post-mortem is still
       readable after the machine halts. Best-effort: if the root
       filesystem isn't mounted yet, or if the panic was triggered
       inside the heap or fs code we'd have to re-enter here, the
       flush silently no-ops. */
    boot_log_flush("/panic.log");
    kprintf("System halted.\n");
    for (;;) __asm__ volatile ("hlt");
}
