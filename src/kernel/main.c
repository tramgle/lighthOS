/*
 * L5 kernel_main — full kernel init + spawn a real user binary
 * via process_spawn_from_memory, handle its syscalls through the
 * new dispatcher, wait on it, print exit code, shutdown.
 *
 * This replaces the L4 "one-shot iretq + hand-crafted opcodes"
 * scaffolding (hello_l4.s / L4_OP_* sentinels). main.c.pre_l5
 * remains in the tree as the pre-port reference; when process.c,
 * vfs, drivers, etc. are all re-ported this file absorbs it back.
 */

#include "include/types.h"
#include "include/multiboot.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/heap.h"
#include "kernel/gdt.h"
#include "kernel/tss.h"
#include "kernel/idt.h"
#include "kernel/isr.h"
#include "kernel/pic.h"
#include "kernel/elf.h"
#include "kernel/task.h"
#include "kernel/process.h"
#include "kernel/syscall.h"
#include "kernel/timer.h"
#include "drivers/vga.h"
#include "drivers/serial.h"

extern char stack_top;

static void *multiboot_first_module(multiboot_info_t *mbi, uint64_t *size_out) {
    if (!(mbi->flags & MULTIBOOT_FLAG_MODS) || mbi->mods_count == 0) return 0;
    /* mbi->mods_addr is a physical address; reach it through HHDM. */
    multiboot_mod_t *m = (multiboot_mod_t *)phys_to_virt_low(mbi->mods_addr);
    *size_out = (uint64_t)(m->mod_end - m->mod_start);
    return (void *)phys_to_virt_low(m->mod_start);
}

/* Kernel heap must be reachable regardless of which PML4 is live.
   Each per-process PML4 only copies the kernel half (PML4[256..511]);
   PML4[0..255] is private to the user. So the heap can't live at a
   low-half VA (that's what broke the first L5 boot — CR3 switch made
   heap unreachable). Instead we place it at higher-half VMA backed
   by phys KHEAP_PHYS, inside the 1 GiB boot-map. */
#define KHEAP_PHYS 0x01000000ULL                        /* 16 MiB */
#define KHEAP_VA   (0xFFFFFFFF80000000ULL + KHEAP_PHYS)
#define KHEAP_SIZE (8 * 1024 * 1024)                    /* 8 MiB */

void kernel_main(uint32_t magic, multiboot_info_t *mbi) {
    vga_init();
    serial_init();
    boot_log_enable();

    kprintf("\n================================\n");
    kprintf("LighthOS x86_64: booting\n");
    kprintf("================================\n");

    if (magic != MULTIBOOT_MAGIC) {
        kprintf("bad multiboot magic; halting.\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    pmm_init(mbi);
    vmm_init();
    /* vmm_init dropped the low-half identity map; rebase the mbi
       pointer (and its embedded phys pointers, as they're used)
       through HHDM so post-vmm code can still read it. */
    mbi = (multiboot_info_t *)phys_to_virt_low((uint64_t)(uintptr_t)mbi);
    pmm_reserve_range(KHEAP_PHYS, KHEAP_SIZE);
    heap_init(KHEAP_VA, KHEAP_SIZE);

    gdt_init();
    tss_init((uint64_t)(uintptr_t)&stack_top);
    pic_init();
    idt_init();
    timer_init(100);
    serial_init_irq();
    syscall_init();

    task_init();
    process_init();

    /* Spawn the first multiboot module as the init process. */
    uint64_t mod_size = 0;
    void *mod = multiboot_first_module(mbi, &mod_size);
    if (!mod) { kprintf("no init module loaded\n"); goto halt; }

    char *const argv[] = { (char *)"hello", 0 };
    int pid = process_spawn_from_memory("hello", mod, mod_size, argv);
    if (pid < 0) { kprintf("spawn failed\n"); goto halt; }
    kprintf("[main] spawned pid %d\n", pid);

    task_enable_scheduling();
    __asm__ volatile ("sti");

    int status = 0;
    process_waitpid((uint32_t)pid, &status);
    kprintf("[main] pid %d exited with code %d\n", pid, status);

    /* Shutdown via QEMU isa-debug-exit. */
    __asm__ volatile ("outw %0, %1" :: "a"((uint16_t)0x2000), "Nd"((uint16_t)0x604));

halt:
    for (;;) __asm__ volatile ("cli; hlt");
}
