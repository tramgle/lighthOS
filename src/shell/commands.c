#include "shell/commands.h"
#include "shell/shell.h"
#include "include/io.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "drivers/vga.h"
#include "mm/pmm.h"
#include "mm/heap.h"
#include "fs/vfs.h"
#include "fs/ramfs.h"
#include "fs/simplefs.h"
#include "fs/blkdev.h"
#include "drivers/ramdisk.h"
#include "kernel/task.h"
#include "kernel/process.h"
#include "kernel/timer.h"
#include "mm/vmm.h"

/* Forward declarations */
static void cmd_help(int argc, char **argv);
static void cmd_echo(int argc, char **argv);
static void cmd_clear(int argc, char **argv);
static void cmd_exit(int argc, char **argv);
static void cmd_pwd(int argc, char **argv);
static void cmd_cd(int argc, char **argv);
static void cmd_ls(int argc, char **argv);
static void cmd_cat(int argc, char **argv);
static void cmd_write(int argc, char **argv);
static void cmd_rm(int argc, char **argv);
static void cmd_mkdir(int argc, char **argv);
static void cmd_mount(int argc, char **argv);
static void cmd_umount(int argc, char **argv);
static void cmd_free(int argc, char **argv);
static void cmd_mkfs(int argc, char **argv);
static void cmd_ps(int argc, char **argv);
static void cmd_exec(int argc, char **argv);
static void cmd_uptime(int argc, char **argv);
static void cmd_hexdump(int argc, char **argv);
static void cmd_pagemap(int argc, char **argv);
static void cmd_regions(int argc, char **argv);

static const command_t command_table[] = {
    {"help",   "Show available commands",     cmd_help},
    {"echo",   "Print text",                  cmd_echo},
    {"clear",  "Clear the screen",            cmd_clear},
    {"exit",   "Halt the system",             cmd_exit},
    {"pwd",    "Print working directory",      cmd_pwd},
    {"cd",     "Change directory",             cmd_cd},
    {"ls",     "List directory contents",      cmd_ls},
    {"cat",    "Display file contents",        cmd_cat},
    {"write",  "Write text to a file",         cmd_write},
    {"rm",     "Remove a file",                cmd_rm},
    {"mkdir",  "Create a directory",           cmd_mkdir},
    {"mount",  "Mount a filesystem",           cmd_mount},
    {"umount", "Unmount a filesystem",         cmd_umount},
    {"free",   "Show memory statistics",       cmd_free},
    {"ps",     "List running processes",          cmd_ps},
    {"exec",   "Run an ELF program",             cmd_exec},
    {"uptime", "Show system uptime",            cmd_uptime},
    {"mkfs",   "Format a block device",        cmd_mkfs},
    {"hexdump","Dump memory: hexdump <hex> [n]", cmd_hexdump},
    {"pagemap","Decode PDE/PTE for a vaddr",    cmd_pagemap},
    {"regions","Show PMM free/used regions",    cmd_regions},
};

#define NUM_COMMANDS (sizeof(command_table) / sizeof(command_table[0]))

const command_t *commands_get_table(int *count) {
    *count = NUM_COMMANDS;
    return command_table;
}

static void cmd_help(int argc, char **argv) {
    (void)argc; (void)argv;
    kprintf("Available commands:\n");
    for (uint32_t i = 0; i < NUM_COMMANDS; i++) {
        kprintf("  %-8s %s\n", command_table[i].name, command_table[i].description);
    }
}

static void cmd_echo(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (i > 1) kprintf(" ");
        kprintf("%s", argv[i]);
    }
    kprintf("\n");
}

static void cmd_clear(int argc, char **argv) {
    (void)argc; (void)argv;
    vga_clear();
}

static void cmd_exit(int argc, char **argv) {
    (void)argc; (void)argv;
    kprintf("Saving history...\n");
    shell_save_history();
    kprintf("Shutting down...\n");
    __asm__ volatile ("cli");
    /* ACPI shutdown: QEMU i440fx/PIIX4 chipset */
    outw(0x604, 0x2000);
    /* Bochs/older QEMU fallback */
    outw(0xB004, 0x2000);
    /* If neither worked, halt */
    for (;;) __asm__ volatile ("hlt");
}

static void cmd_pwd(int argc, char **argv) {
    (void)argc; (void)argv;
    kprintf("%s\n", shell_get_cwd());
}

static void cmd_cd(int argc, char **argv) {
    if (argc < 2) {
        shell_set_cwd("/");
        return;
    }

    const char *target = argv[1];
    char resolved[VFS_MAX_PATH];

    if (strcmp(target, "..") == 0) {
        /* Go up one level */
        char parent[VFS_MAX_PATH];
        strncpy(parent, shell_get_cwd(), VFS_MAX_PATH - 1);
        parent[VFS_MAX_PATH - 1] = '\0';

        /* Find last slash */
        int len = strlen(parent);
        if (len > 1) {
            /* Remove trailing component */
            for (int i = len - 1; i > 0; i--) {
                if (parent[i] == '/') {
                    parent[i] = '\0';
                    break;
                }
            }
            /* If we stripped everything after first /, result is "/" */
            if (parent[0] == '/' && parent[1] == '\0') {
                /* already "/" */
            } else if (parent[0] == '\0') {
                parent[0] = '/';
                parent[1] = '\0';
            }
        }
        shell_set_cwd(parent);
        return;
    }

    shell_resolve_path(target, resolved, VFS_MAX_PATH);

    /* Verify it exists and is a directory */
    struct vfs_stat st;
    if (vfs_stat(resolved, &st) != 0) {
        kprintf("cd: %s: not found\n", resolved);
        return;
    }
    if (st.type != VFS_DIR) {
        kprintf("cd: %s: not a directory\n", resolved);
        return;
    }

    shell_set_cwd(resolved);
}

static void cmd_ls(int argc, char **argv) {
    char path[VFS_MAX_PATH];
    if (argc > 1) {
        shell_resolve_path(argv[1], path, VFS_MAX_PATH);
    } else {
        strncpy(path, shell_get_cwd(), VFS_MAX_PATH - 1);
        path[VFS_MAX_PATH - 1] = '\0';
    }

    char name[VFS_MAX_NAME];
    uint32_t type;
    uint32_t idx = 0;

    while (vfs_readdir(path, idx, name, &type) == 0) {
        if (type == VFS_DIR) {
            kprintf("  %s/\n", name);
        } else {
            kprintf("  %s", name);
            /* Try to get size */
            char full[VFS_MAX_PATH];
            if (strcmp(path, "/") == 0) {
                full[0] = '/';
                strncpy(full + 1, name, VFS_MAX_PATH - 2);
            } else {
                strncpy(full, path, VFS_MAX_PATH - 1);
                strcat(full, "/");
                strcat(full, name);
            }
            full[VFS_MAX_PATH - 1] = '\0';
            struct vfs_stat st;
            if (vfs_stat(full, &st) == 0) {
                kprintf("  (%u bytes)", st.size);
            }
            kprintf("\n");
        }
        idx++;
    }
    if (idx == 0) {
        kprintf("  (empty)\n");
    }
}

static void cmd_cat(int argc, char **argv) {
    if (argc < 2) {
        kprintf("Usage: cat <path>\n");
        return;
    }
    char path[VFS_MAX_PATH];
    shell_resolve_path(argv[1], path, VFS_MAX_PATH);

    struct vfs_stat st;
    if (vfs_stat(path, &st) != 0) {
        kprintf("cat: %s: not found\n", argv[1]);
        return;
    }
    if (st.type == VFS_DIR) {
        kprintf("cat: %s: is a directory\n", argv[1]);
        return;
    }
    if (st.size == 0) return;

    char buf[512];
    off_t offset = 0;
    while (offset < st.size) {
        size_t chunk = st.size - offset;
        if (chunk > sizeof(buf) - 1) chunk = sizeof(buf) - 1;
        ssize_t n = vfs_read(path, buf, chunk, offset);
        if (n <= 0) break;
        buf[n] = '\0';
        kprintf("%s", buf);
        offset += n;
    }
    kprintf("\n");
}

static void cmd_write(int argc, char **argv) {
    if (argc < 3) {
        kprintf("Usage: write <path> <text...>\n");
        return;
    }
    char path[VFS_MAX_PATH];
    shell_resolve_path(argv[1], path, VFS_MAX_PATH);

    char text[512];
    text[0] = '\0';
    for (int i = 2; i < argc; i++) {
        if (i > 2) strcat(text, " ");
        strcat(text, argv[i]);
    }

    struct vfs_stat st;
    if (vfs_stat(path, &st) != 0) {
        if (vfs_create(path, VFS_FILE) != 0) {
            kprintf("write: cannot create %s\n", argv[1]);
            return;
        }
    }

    ssize_t n = vfs_write(path, text, strlen(text), 0);
    if (n < 0) {
        kprintf("write: error writing to %s\n", argv[1]);
    }
}

static void cmd_rm(int argc, char **argv) {
    if (argc < 2) {
        kprintf("Usage: rm <path>\n");
        return;
    }
    char path[VFS_MAX_PATH];
    shell_resolve_path(argv[1], path, VFS_MAX_PATH);
    if (vfs_unlink(path) != 0) {
        kprintf("rm: cannot remove %s\n", argv[1]);
    }
}

static void cmd_mkdir(int argc, char **argv) {
    if (argc < 2) {
        kprintf("Usage: mkdir <path>\n");
        return;
    }
    char path[VFS_MAX_PATH];
    shell_resolve_path(argv[1], path, VFS_MAX_PATH);
    if (vfs_mkdir(path) != 0) {
        kprintf("mkdir: cannot create directory %s\n", argv[1]);
    }
}

static void cmd_mount(int argc, char **argv) {
    if (argc < 3) {
        kprintf("Usage: mount <device> <path>\n");
        kprintf("  device: ata0, ramdisk:<size_kb>, ramfs\n");
        return;
    }

    const char *device = argv[1];
    const char *mpath  = argv[2];

    if (strcmp(device, "ramfs") == 0) {
        vfs_node_t *root = ramfs_init();
        if (!root) { kprintf("mount: ramfs init failed\n"); return; }
        if (vfs_mount(mpath, ramfs_get_ops(), root, NULL) != 0) {
            kprintf("mount: failed to mount at %s\n", mpath);
        }
    } else if (strncmp(device, "ramdisk:", 8) == 0) {
        uint32_t size_kb = 0;
        const char *p = device + 8;
        while (*p >= '0' && *p <= '9') {
            size_kb = size_kb * 10 + (*p - '0');
            p++;
        }
        if (size_kb == 0) size_kb = 64;

        static int rd_count = 0;
        char name[32];
        name[0] = 'r'; name[1] = 'd';
        name[2] = '0' + rd_count;
        name[3] = '\0';
        rd_count++;

        blkdev_t *rd = ramdisk_create(name, size_kb * 1024);
        if (!rd) { kprintf("mount: ramdisk create failed\n"); return; }
        simplefs_format(rd);
        vfs_node_t *root = simplefs_mount(rd);
        if (!root) { kprintf("mount: simplefs mount failed\n"); return; }
        if (vfs_mount(mpath, simplefs_get_ops(), root, root->private_data) != 0) {
            kprintf("mount: failed to mount at %s\n", mpath);
        }
    } else {
        blkdev_t *dev = blkdev_get(device);
        if (!dev) { kprintf("mount: device '%s' not found\n", device); return; }
        vfs_node_t *root = simplefs_mount(dev);
        if (!root) { kprintf("mount: simplefs mount failed on %s\n", device); return; }
        if (vfs_mount(mpath, simplefs_get_ops(), root, root->private_data) != 0) {
            kprintf("mount: failed to mount at %s\n", mpath);
        }
    }
}

static void cmd_umount(int argc, char **argv) {
    if (argc < 2) {
        kprintf("Usage: umount <path>\n");
        return;
    }
    if (vfs_umount(argv[1]) != 0) {
        kprintf("umount: failed to unmount %s\n", argv[1]);
    }
}

static void cmd_free(int argc, char **argv) {
    (void)argc; (void)argv;
    uint32_t total = pmm_get_total_count();
    uint32_t free  = pmm_get_free_count();
    uint32_t used  = total - free;
    kprintf("Physical memory:\n");
    kprintf("  Total: %u pages (%u KB)\n", total, (total * 4096) / 1024);
    kprintf("  Used:  %u pages (%u KB)\n", used,  (used * 4096) / 1024);
    kprintf("  Free:  %u pages (%u KB)\n", free,  (free * 4096) / 1024);
    kprintf("Kernel heap:\n");
    kprintf("  Used:  %u bytes\n", heap_get_used());
    kprintf("  Free:  %u bytes\n", heap_get_free());
}

static void cmd_ps(int argc, char **argv) {
    (void)argc; (void)argv;
    process_list_all();
}

static void cmd_uptime(int argc, char **argv) {
    (void)argc; (void)argv;
    uint32_t ticks = timer_get_ticks();
    uint32_t secs = ticks / 100;
    uint32_t mins = secs / 60;
    kprintf("Uptime: %um %us (%u ticks)\n", mins, secs % 60, ticks);
}

static void cmd_exec(int argc, char **argv) {
    if (argc < 2) {
        kprintf("Usage: exec <path>\n");
        return;
    }
    char path[VFS_MAX_PATH];
    shell_resolve_path(argv[1], path, VFS_MAX_PATH);

    int pid = process_spawn(path, 0);
    if (pid < 0) {
        kprintf("exec: failed to run %s\n", argv[1]);
        return;
    }
    /* Wait for the child to finish (foreground execution) */
    int status = 0;
    process_waitpid((uint32_t)pid, &status);
}

static uint32_t parse_hex(const char *s) {
    uint32_t v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    while (*s) {
        char c = *s++;
        uint32_t d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        v = (v << 4) | d;
    }
    return v;
}

static uint32_t parse_dec(const char *s) {
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return v;
}

static void cmd_hexdump(int argc, char **argv) {
    if (argc < 2) {
        kprintf("Usage: hexdump <hexaddr> [count]\n");
        return;
    }
    uint32_t addr = parse_hex(argv[1]);
    uint32_t n = (argc > 2) ? parse_dec(argv[2]) : 128;

    uint32_t *pd = task_current_pd();
    for (uint32_t off = 0; off < n; off += 16) {
        uint32_t row = addr + off;
        uint32_t page = row & ~(PAGE_SIZE - 1);
        if (pd && vmm_get_physical_in(pd, page) == 0) {
            kprintf("%x: <unmapped>\n", row);
            /* Skip to next page boundary */
            uint32_t skip = PAGE_SIZE - (row - page);
            if (skip > n - off) break;
            off += skip - 16;
            continue;
        }
        kprintf("%x: ", row);
        uint32_t line = 16;
        if (off + line > n) line = n - off;
        for (uint32_t i = 0; i < line; i++) {
            uint8_t b = ((uint8_t *)row)[i];
            kprintf("%x%x ", (b >> 4) & 0xF, b & 0xF);
        }
        for (uint32_t i = line; i < 16; i++) kprintf("   ");
        kprintf(" |");
        for (uint32_t i = 0; i < line; i++) {
            char b = ((char *)row)[i];
            if (b >= ' ' && b < 127) kprintf("%c", b);
            else kprintf(".");
        }
        kprintf("|\n");
    }
}

static void cmd_pagemap(int argc, char **argv) {
    if (argc < 2) {
        kprintf("Usage: pagemap <hexaddr>\n");
        return;
    }
    uint32_t va = parse_hex(argv[1]);
    uint32_t pdi = va >> 22;
    uint32_t pti = (va >> 12) & 0x3FF;
    uint32_t off = va & 0xFFF;

    uint32_t *pd = task_current_pd();
    if (!pd) { kprintf("no current PD\n"); return; }

    kprintf("vaddr 0x%x  pd_idx=%u  pt_idx=%u  offset=0x%x\n", va, pdi, pti, off);
    uint32_t pde = pd[pdi];
    kprintf("PDE[%u]=0x%x  ", pdi, pde);
    if (!(pde & VMM_FLAG_PRESENT)) {
        kprintf("not present\n");
        return;
    }
    kprintf("{P W=%u U=%u PT=0x%x}\n",
            (pde >> 1) & 1, (pde >> 2) & 1, pde & 0xFFFFF000);

    uint32_t *pt = (uint32_t *)(pde & 0xFFFFF000);
    uint32_t pte = pt[pti];
    kprintf("PTE[%u]=0x%x  ", pti, pte);
    if (!(pte & VMM_FLAG_PRESENT)) {
        kprintf("not present\n");
        return;
    }
    kprintf("{P W=%u U=%u frame=0x%x}  phys=0x%x\n",
            (pte >> 1) & 1, (pte >> 2) & 1, pte & 0xFFFFF000,
            (pte & 0xFFFFF000) | off);
}

static void regions_cb(uint32_t start, uint32_t len, bool used) {
    uint32_t start_addr = start * PAGE_SIZE;
    uint32_t end_addr   = (start + len) * PAGE_SIZE;
    kprintf("  0x%x - 0x%x  %s  %u frames (%u KB)\n",
            start_addr, end_addr, used ? "USED" : "FREE",
            len, (len * PAGE_SIZE) / 1024);
}

static void cmd_regions(int argc, char **argv) {
    (void)argc; (void)argv;
    kprintf("PMM regions:\n");
    pmm_region_iter(regions_cb);
}

static void cmd_mkfs(int argc, char **argv) {
    if (argc < 2) {
        kprintf("Usage: mkfs <device>\n");
        return;
    }
    blkdev_t *dev = blkdev_get(argv[1]);
    if (!dev) {
        kprintf("mkfs: device '%s' not found\n", argv[1]);
        return;
    }
    if (simplefs_format(dev) == 0) {
        kprintf("Formatted %s with simplefs\n", argv[1]);
    } else {
        kprintf("mkfs: format failed\n");
    }
}
