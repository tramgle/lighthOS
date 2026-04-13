#include "include/types.h"
#include "include/multiboot.h"
#include "drivers/vga.h"
#include "drivers/serial.h"
#include "drivers/keyboard.h"
#include "drivers/ata.h"
#include "lib/kprintf.h"
#include "kernel/gdt.h"
#include "kernel/idt.h"
#include "kernel/pic.h"
#include "kernel/timer.h"
#include "kernel/panic.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/heap.h"
#include "fs/vfs.h"
#include "fs/ramfs.h"
#include "fs/simplefs.h"
#include "shell/shell.h"

extern uint32_t _kernel_end;

#define HEAP_SIZE (1024 * 1024)

void kernel_main(uint32_t magic, multiboot_info_t *mbi) {
    vga_init();
    serial_init();

    kprintf("VibeOS booting...\n\n");

    if (magic != MULTIBOOT_MAGIC) {
        panic("Invalid multiboot magic number");
    }

    serial_printf("[boot] Multiboot magic OK: 0x%x\n", magic);
    kprintf("Memory: lower=%uKB upper=%uKB\n", mbi->mem_lower, mbi->mem_upper);

    gdt_init();
    serial_printf("[boot] GDT initialized\n");

    pic_init();
    serial_printf("[boot] PIC remapped\n");

    idt_init();
    serial_printf("[boot] IDT initialized\n");

    timer_init(100);
    serial_printf("[boot] PIT timer at 100Hz\n");

    keyboard_init();
    serial_printf("[boot] Keyboard driver ready\n");

    serial_init_irq();
    serial_printf("[boot] Serial input ready (IRQ4)\n");

    __asm__ volatile ("sti");
    serial_printf("[boot] Interrupts enabled\n");

    pmm_init(mbi);
    serial_printf("[boot] PMM initialized\n");

    vmm_init();
    serial_printf("[boot] VMM initialized (paging enabled)\n");

    uint32_t heap_start = ((uint32_t)&_kernel_end + 0x20000) & ~(PAGE_SIZE - 1);
    heap_init(heap_start, HEAP_SIZE);
    serial_printf("[boot] Heap initialized\n");

    /* Initialize VFS and mount root ramfs */
    vfs_init();
    vfs_node_t *ramfs_root = ramfs_init();
    if (!ramfs_root) panic("Failed to init ramfs");
    vfs_mount("/", ramfs_get_ops(), ramfs_root, NULL);
    serial_printf("[boot] Root filesystem mounted (ramfs)\n");

    /* Probe ATA and mount disk if present */
    ata_init();
    if (ata_get_device()) {
        vfs_node_t *sfs_root = simplefs_mount(ata_get_device());
        if (sfs_root) {
            /* Create /disk mount point in ramfs */
            vfs_mkdir("/disk");
            vfs_mount("/disk", simplefs_get_ops(), sfs_root, sfs_root->private_data);
            serial_printf("[boot] Disk mounted at /disk\n");
        }
    }

    kprintf("Welcome to VibeOS!\n");
    kprintf("Free memory: %u KB\n", (pmm_get_free_count() * PAGE_SIZE) / 1024);
    kprintf("Type 'help' for available commands.\n\n");

    shell_run();
}
