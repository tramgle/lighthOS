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
#include "fs/fstab.h"
#include "kernel/task.h"
#include "shell/shell.h"

extern uint32_t _kernel_end;

#define HEAP_SIZE (8 * 1024 * 1024)

void kernel_main(uint32_t magic, multiboot_info_t *mbi) {
    vga_init();
    serial_init();
    /* Capture everything from here forward into an in-memory log.
       Once the root fs is mounted we'll flush to /boot.log; until
       then the buffer lives in BSS so it's safe to use before
       heap_init. */
    boot_log_enable();

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

    /* Load GRUB modules.
       grub.cfg passes names explicitly:   module /boot/hello hello
       cmdline becomes "/boot/hello hello". We use the LAST
       space-separated token as the install path. If that token
       contains a '/' it's used as-is (e.g. "tests/pipes.vsh" →
       /tests/pipes.vsh, with /tests auto-created). Otherwise the
       module is dropped into /bin/<name>. */
    vfs_mkdir("/bin");
    if (mbi->flags & MULTIBOOT_FLAG_MODS && mbi->mods_count > 0) {
        multiboot_mod_t *mods = (multiboot_mod_t *)mbi->mods_addr;
        for (uint32_t i = 0; i < mbi->mods_count; i++) {
            uint32_t size = mods[i].mod_end - mods[i].mod_start;
            const char *cmdline = (const char *)mods[i].cmdline;

            /* Copy the last token of cmdline into `token`. */
            char token[VFS_MAX_PATH];
            token[0] = '\0';
            if (cmdline && *cmdline) {
                const char *last_token = cmdline;
                for (const char *p = cmdline; *p; p++) {
                    if (*p == ' ') last_token = p + 1;
                }
                int j = 0;
                while (last_token[j] && last_token[j] != ' ' &&
                       j < (int)sizeof token - 1) {
                    token[j] = last_token[j];
                    j++;
                }
                token[j] = '\0';
            }
            if (token[0] == '\0') {
                token[0] = 'm'; token[1] = 'o'; token[2] = 'd';
                token[3] = '0' + (char)i; token[4] = '\0';
            }

            /* Does the token carry its own directory? */
            int has_slash = 0;
            for (int k = 0; token[k]; k++) if (token[k] == '/') { has_slash = 1; break; }

            char path[VFS_MAX_PATH];
            if (has_slash) {
                /* Use token verbatim as the path, prefixing '/' if it
                   doesn't start with one. Auto-create the parent
                   directory so grub-test.cfg can use nested paths like
                   "tests/pipes.vsh" without hand-rolling mkdirs. */
                int pi = 0;
                if (token[0] != '/') path[pi++] = '/';
                int k = 0;
                while (token[k] && pi < (int)sizeof path - 1) path[pi++] = token[k++];
                path[pi] = '\0';
                /* Auto-mkdir the parent. Only handles one level, which
                   is enough for /tests/. */
                int last_slash = -1;
                for (int s = 0; path[s]; s++) if (path[s] == '/') last_slash = s;
                if (last_slash > 0) {
                    char parent[VFS_MAX_PATH];
                    for (int s = 0; s < last_slash; s++) parent[s] = path[s];
                    parent[last_slash] = '\0';
                    vfs_mkdir(parent);  /* idempotent */
                }
            } else {
                strcpy(path, "/bin/");
                strcat(path, token);
            }

            vfs_create(path, VFS_FILE);
            vfs_write(path, (void *)mods[i].mod_start, size, 0);
            serial_printf("[boot] Loaded module (%u bytes, cmdline='%s', phys=0x%x) -> %s\n",
                          size, cmdline ? cmdline : "(null)", mods[i].mod_start, path);
        }
    }

    /* Probe ATA and carve out the FAT32 partition starting at LBA 2048
       (sectors 0..2047 are MBR + stage2 + kernel for the bootdisk
       flow). Partition size = total disk minus the reserved region.
       fstab then mounts this as the root filesystem. */
    ata_init();
    if (ata_get_device()) {
        blkdev_t *root_dev = ata_get_device();
        uint32_t part_start = 2048;
        uint32_t part_size  = root_dev->total_sectors > part_start
                              ? root_dev->total_sectors - part_start : 0;
        if (part_size > 0) {
            blkdev_partition(root_dev, part_start, part_size, "ata0p0");
        }
    }

    /* Primary fstab: try ramfs /etc/fstab first (shipped as a
       multiboot module in the ISO), fall back to a built-in default
       when it's missing. ISO boots keep ramfs at '/' and mount the
       disk (if attached) at /disk for dev work. Self-contained
       bootdisk boots replace '/' with the install FAT. */
    struct vfs_stat fstab_st;
    int mounted = (vfs_stat("/etc/fstab", &fstab_st) == 0)
                      ? fstab_mount_file("/etc/fstab")
                      : fstab_mount_defaults();
    serial_printf("[boot] primary fstab mounted %d entry(ies)\n", mounted);

    /* Flush early boot log to /boot.log on a writable fs. For the
       bootdisk case the install FAT is now mounted at '/' directly,
       so this lands at /boot.log on the install partition; ISO boots
       write to ramfs (ephemeral) which is fine. */
    boot_log_flush("/boot.log");
    serial_printf("[boot] boot log flushed to /boot.log\n");

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

    /* Shell selection: always /bin/shell. Installed-system boots have
       the FAT mounted at '/' so /bin/shell is the installed binary;
       ISO boots have ramfs at '/' with /bin/shell loaded from
       multiboot modules. No chroot layer in either case — the
       "install = /disk + chroot" model was an accidental detour. */
    struct vfs_stat shell_st, tests_st;
    const char *shell_path = NULL;
    int in_test_mode = (vfs_stat("/tests", &tests_st) == 0);

    if (vfs_stat("/bin/shell", &shell_st) == 0) {
        shell_path = "/bin/shell";
        kprintf(in_test_mode ? "Starting test-mode shell...\n\n"
                             : "Starting user shell...\n\n");
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
