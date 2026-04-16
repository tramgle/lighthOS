#include "kernel/panic.h"
#include "kernel/debug.h"
#include "lib/kprintf.h"
#include "fs/vfs.h"

void panic(const char *msg) {
    __asm__ volatile ("cli");
    kprintf("\n*** KERNEL PANIC: %s ***\n", msg);
    debug_backtrace((uint32_t)__builtin_frame_address(0));

    /* Best-effort: flush the in-memory boot log (which captures every
       serial_printf including the panic banner + backtrace above)
       to /panic.log on whatever owns '/'. If the panic fired before
       the root fs is mounted, or the fs is broken, the flush simply
       fails and we continue to the halt. Log-to-file runs with IRQs
       still disabled; the FAT driver does synchronous ATA I/O so
       this is fine for a single write from kernel context. */
    struct vfs_stat st;
    if (vfs_stat("/", &st) == 0) {
        boot_log_flush("/panic.log");
        kprintf("Panic log written to /panic.log\n");
    }

    kprintf("System halted.\n");
    for (;;) {
        __asm__ volatile ("hlt");
    }
}
