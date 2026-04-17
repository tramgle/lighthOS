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
#include "drivers/keyboard.h"
#include "drivers/console.h"
#include "drivers/ata.h"
#include "fs/vfs.h"
#include "fs/ramfs.h"
#include "fs/blkdev.h"
#include "fs/fstab.h"

extern char stack_top;

/* FAT install partition on a LighthOS disk starts at LBA 2048.
   The custom MBR/stage2 build (Makefile bootdisk target) stamps
   MBR + stage2 + kernel into LBAs 0..2047, leaving the FAT
   partition untouched. The kernel treats this offset as a
   well-known constant rather than reading an MBR partition
   table. */
#define FAT_PARTITION_LBA 2048

/* Pull each multiboot module into ramfs at the path given by its
   cmdline. GRUB entries like
       module /boot/hello  /bin/hello
   become a file at /bin/hello with the module bytes as contents.
   Missing cmdlines fall back to /mod<N>. */
static void populate_ramfs_from_modules(multiboot_info_t *mbi) {
    if (!(mbi->flags & MULTIBOOT_FLAG_MODS)) return;
    multiboot_mod_t *mods = (multiboot_mod_t *)phys_to_virt_low(mbi->mods_addr);
    for (uint32_t i = 0; i < mbi->mods_count; i++) {
        const char *cmdline = mods[i].cmdline
            ? (const char *)phys_to_virt_low(mods[i].cmdline)
            : "/mod";
        /* Some loaders prepend the module filename before the user
           cmdline separated by a space — take the first whitespace-
           delimited token as the target path. */
        char path[VFS_MAX_PATH];
        int pi = 0;
        if (cmdline[0] != '/') path[pi++] = '/';
        for (int j = 0; cmdline[j] && cmdline[j] != ' ' && pi < VFS_MAX_PATH - 1; j++) {
            path[pi++] = cmdline[j];
        }
        path[pi] = 0;

        /* Ensure parent directories exist — support one level for now. */
        for (int j = 1; j < pi; j++) {
            if (path[j] == '/') {
                char saved = path[j];
                path[j] = 0;
                struct vfs_stat dst;
                if (vfs_stat(path, &dst) != 0) vfs_mkdir(path);
                path[j] = saved;
            }
        }

        void *bytes = (void *)phys_to_virt_low(mods[i].mod_start);
        uint64_t size = (uint64_t)(mods[i].mod_end - mods[i].mod_start);
        vfs_create(path, VFS_FILE);
        vfs_write(path, bytes, (size_t)size, 0);
        kprintf("[mod] registered %s (%lu bytes)\n", path, size);
    }
}

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
#define KHEAP_SIZE (32 * 1024 * 1024)                   /* 32 MiB */

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
    keyboard_init();
    syscall_init();

    task_init();
    process_init();

    /* VFS always starts with a ramfs at '/'. If multiboot modules
       were provided (ISO boot path), stage them into it. If an ATA
       drive is present (bootdisk path), register a FAT partition
       at LBA FAT_PARTITION_LBA and let fstab swap the ramfs out
       for the FAT root. vfs_mount's replace-on-collision makes the
       handoff clean. */
    vfs_init();
    vfs_node_t *ramroot = ramfs_init();
    if (ramroot) vfs_mount("/", ramroot->ops, ramroot, 0);

    int have_modules = (mbi->flags & MULTIBOOT_FLAG_MODS) && mbi->mods_count > 0;
    if (have_modules) populate_ramfs_from_modules(mbi);

    ata_init();
    blkdev_t *ata = blkdev_get("ata0");
    if (ata) {
        uint32_t part_sects = ata->total_sectors > FAT_PARTITION_LBA
                              ? ata->total_sectors - FAT_PARTITION_LBA : 0;
        blkdev_partition(ata, FAT_PARTITION_LBA, part_sects, "ata0p0");
        if (have_modules) {
            /* ISO flow: ramfs already owns '/'. Mount the FAT
               partition at /disk for tests that exercise mount/
               umount round-trips. */
            vfs_mkdir("/disk");
            fstab_do_mount("ata0p0", "/disk", "fat", "rw");
        } else {
            /* Bootdisk flow: FAT is the root filesystem. */
            fstab_mount_defaults();
        }
    }

    /* Parse `autorun=<path>` from the multiboot cmdline. When set,
       the kernel spawns that path as init (instead of the first
       module / /bin/init fallback). Used by the test-iso harness
       so a single cmdline line picks the entry point. */
    char autorun[VFS_MAX_PATH] = {0};
    if ((mbi->flags & MULTIBOOT_FLAG_CMDLINE) && mbi->cmdline) {
        const char *cmd = (const char *)phys_to_virt_low(mbi->cmdline);
        const char *p = cmd;
        while (*p) {
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) break;
            const char *tok = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            if (p - tok > 8 && tok[0] == 'a' && tok[1] == 'u' &&
                tok[2] == 't' && tok[3] == 'o' && tok[4] == 'r' &&
                tok[5] == 'u' && tok[6] == 'n' && tok[7] == '=') {
                const char *val = tok + 8;
                int n = 0;
                while (val < p && n < (int)sizeof(autorun) - 1) autorun[n++] = *val++;
                autorun[n] = 0;
            }
        }
    }

    int pid = -1;
    char *const argv[] = { (char *)"init", 0 };
    if (autorun[0]) {
        kprintf("[main] autorun=%s\n", autorun);
        pid = process_spawn_from_path(autorun, argv);
    } else if (have_modules) {
        uint64_t mod_size = 0;
        void *mod = multiboot_first_module(mbi, &mod_size);
        if (mod) pid = process_spawn_from_memory("init", mod, mod_size, argv);
    } else {
        pid = process_spawn_from_path("/bin/init", argv);
        if (pid < 0) pid = process_spawn_from_path("/bin/hello", argv);
    }
    if (pid < 0) { kprintf("spawn failed; no init found\n"); goto halt; }
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
