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
    {"mkfs",   "Format a block device",        cmd_mkfs},
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
