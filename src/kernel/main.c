#ifdef RUN_TESTS
#include "test/test.h"
#endif
#include "include/types.h"
#include "include/multiboot.h"
#include "drivers/vga.h"
#include "drivers/serial.h"
#include "drivers/keyboard.h"
#include "drivers/ata.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "kernel/gdt.h"
#include "kernel/idt.h"
#include "kernel/pic.h"
#include "kernel/timer.h"
#include "kernel/panic.h"
#include "kernel/tss.h"
#include "kernel/syscall.h"
#include "kernel/process.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/heap.h"
#include "fs/vfs.h"
#include "fs/ramfs.h"
#include "fs/simplefs.h"
#include "fs/fat.h"
#include "kernel/task.h"
#include "shell/shell.h"

extern uint32_t _kernel_end;

#define HEAP_SIZE (8 * 1024 * 1024)

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

    /* TSS must be set up after GDT. Use boot stack top as initial kernel ESP.
       The boot stack is 16KB defined in boot.s (.bss section). */
    extern uint32_t stack_top;
    tss_init(0x10, (uint32_t)&stack_top);
    serial_printf("[boot] TSS initialized\n");

    pic_init();
    serial_printf("[boot] PIC remapped\n");

    idt_init();
    serial_printf("[boot] IDT initialized\n");

    syscall_init();
    serial_printf("[boot] Syscall gate installed\n");

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

    /* Heap must start past the end of every multiboot module, because
       we'll later vfs_write module contents into kmalloc'd buffers and
       forward memcpy would corrupt the source if dest overlaps source.
       Pick the later of (kernel end + slack) and (highest mod_end),
       page-aligned. */
    uint32_t heap_start = ((uint32_t)&_kernel_end + 0x20000) & ~(PAGE_SIZE - 1);
    if (mbi->flags & MULTIBOOT_FLAG_MODS && mbi->mods_count > 0) {
        multiboot_mod_t *mods = (multiboot_mod_t *)mbi->mods_addr;
        for (uint32_t i = 0; i < mbi->mods_count; i++) {
            uint32_t end = (mods[i].mod_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
            if (end > heap_start) heap_start = end;
        }
    }
    heap_init(heap_start, HEAP_SIZE);
    /* Cordon off the heap from the physical frame allocator, otherwise
       pmm_alloc_frame can hand out frames that kmalloc is already using
       (e.g. kernel task stacks), and user-page writes clobber them. */
    pmm_reserve_range(heap_start, HEAP_SIZE);
    serial_printf("[boot] Heap initialized\n");

    /* Initialize VFS and mount root ramfs */
    vfs_init();
    vfs_node_t *ramfs_root = ramfs_init();
    if (!ramfs_root) panic("Failed to init ramfs");
    vfs_mount("/", ramfs_get_ops(), ramfs_root, NULL);
    serial_printf("[boot] Root filesystem mounted (ramfs)\n");

    /* Load GRUB modules into /bin/.
       grub.cfg should pass names explicitly:  module /boot/hello hello
       Then cmdline is "/boot/hello hello" — we take the last space-separated token. */
    vfs_mkdir("/bin");
    if (mbi->flags & MULTIBOOT_FLAG_MODS && mbi->mods_count > 0) {
        multiboot_mod_t *mods = (multiboot_mod_t *)mbi->mods_addr;
        for (uint32_t i = 0; i < mbi->mods_count; i++) {
            uint32_t size = mods[i].mod_end - mods[i].mod_start;
            const char *cmdline = (const char *)mods[i].cmdline;

            char namebuf[32];
            namebuf[0] = '\0';

            if (cmdline && *cmdline) {
                /* Find the last space-separated token, and within that,
                   the portion after the last '/' */
                const char *last_token = cmdline;
                for (const char *p = cmdline; *p; p++) {
                    if (*p == ' ') last_token = p + 1;
                }
                const char *base = last_token;
                for (const char *p = last_token; *p; p++) {
                    if (*p == '/') base = p + 1;
                }
                int j = 0;
                while (base[j] && base[j] != ' ' && j < 31) {
                    namebuf[j] = base[j];
                    j++;
                }
                namebuf[j] = '\0';
            }

            /* Fallback if empty */
            if (namebuf[0] == '\0') {
                namebuf[0] = 'm'; namebuf[1] = 'o'; namebuf[2] = 'd';
                namebuf[3] = '0' + (char)i; namebuf[4] = '\0';
            }

            char path[VFS_MAX_PATH];
            strcpy(path, "/bin/");
            strcat(path, namebuf);

            vfs_create(path, VFS_FILE);
            vfs_write(path, (void *)mods[i].mod_start, size, 0);
            serial_printf("[boot] Loaded module '%s' (%u bytes, cmdline='%s', phys=0x%x) -> %s\n",
                          namebuf, size, cmdline ? cmdline : "(null)", mods[i].mod_start, path);
        }
    }

    /* Probe ATA and mount disk if present. The first 2048 sectors (1 MB)
       are reserved for MBR + second-stage loader + kernel image, so the
       simplefs partition starts at sector 2048 — we present it to
       simplefs via a partition wrapper so the fs sees sector 0 as its
       own superblock. */
    ata_init();
    if (ata_get_device()) {
        blkdev_t *root_dev = ata_get_device();

        /* Partition 1: simplefs at LBA 2048 (matches MBR partition table). */
        blkdev_t *sfs_part = blkdev_partition(root_dev, 2048, 18432, "part0");
        if (sfs_part) {
            vfs_node_t *sfs_root = simplefs_mount(sfs_part);
            if (sfs_root) {
                vfs_mkdir("/disk");
                vfs_mount("/disk", simplefs_get_ops(), sfs_root, sfs_root->private_data);
                strncpy(sfs_part->mount_path, "/disk", sizeof sfs_part->mount_path - 1);
                strncpy(sfs_part->fs_type, "simplefs", sizeof sfs_part->fs_type - 1);
                sfs_part->read_only = 0;
                serial_printf("[boot] Disk mounted at /disk (simplefs, start=2048)\n");
            }
        }

        /* Partition 2: FAT16 at LBA 20480. Mount at /fat so host tools
           can seed it with `mkfs.fat` + `mcopy` and VibeOS can read
           those files back. Fails silently if the region isn't
           FAT-formatted yet. */
        if (root_dev->total_sectors > 20480) {
            blkdev_t *fat_part = blkdev_partition(root_dev, 20480, 12288, "part1");
            if (fat_part) {
                vfs_node_t *fat_root = fat_mount(fat_part);
                if (fat_root) {
                    vfs_mkdir("/fat");
                    vfs_mount("/fat", fat_get_ops(), fat_root, fat_root->private_data);
                    strncpy(fat_part->mount_path, "/fat", sizeof fat_part->mount_path - 1);
                    strncpy(fat_part->fs_type, "fat16", sizeof fat_part->fs_type - 1);
                    fat_part->read_only = 1;   /* driver is read-only in v1 */
                    serial_printf("[boot] FAT partition mounted at /fat (start=20480)\n");
                }
            }
        }
    }

    /* Initialize tasking and process management */
    task_init();
    process_init();
    serial_printf("[boot] Task/process system initialized\n");

#ifdef RUN_TESTS
    test_run_all();
    for (;;) __asm__ volatile ("hlt");
#else
    kprintf("Welcome to VibeOS!\n");
    kprintf("Free memory: %u KB\n", (pmm_get_free_count() * PAGE_SIZE) / 1024);

    task_enable_scheduling();

    /* Prefer a shell installed on the disk; fall back to the ramfs
       shell; fall back to the built-in kernel shell. */
    struct vfs_stat shell_st;
    const char *shell_path = NULL;
    if (vfs_stat("/disk/bin/shell", &shell_st) == 0) {
        shell_path = "/disk/bin/shell";
        kprintf("Starting disk-installed shell...\n\n");
    } else if (vfs_stat("/bin/shell", &shell_st) == 0) {
        shell_path = "/bin/shell";
        kprintf("Starting user shell...\n\n");
    }

    if (shell_path) {
        int pid = process_spawn(shell_path, 0);
        if (pid >= 0) {
            serial_printf("[init] Spawned %s as pid %d\n", shell_path, pid);
            /* Wait for shell to exit, then respawn (init loop) */
            for (;;) {
                int status;
                process_waitpid((uint32_t)pid, &status);
                serial_printf("[init] Shell exited with %d, respawning...\n", status);
                pid = process_spawn(shell_path, 0);
                if (pid < 0) break;
            }
        }
        kprintf("User shell failed, falling back to built-in shell\n");
    }

    kprintf("Type 'help' for available commands.\n\n");
    process_create("shell", shell_run);
    serial_printf("[boot] Built-in shell started\n");

    /* Task 0 / process 0 becomes the idle task */
    for (;;) {
        __asm__ volatile ("hlt");
    }
#endif
}
