#include "fs/fstab.h"
#include "fs/vfs.h"
#include "fs/blkdev.h"
#include "fs/fat.h"
#include "fs/procfs.h"
#include "mm/heap.h"
#include "lib/string.h"
#include "lib/kprintf.h"

/* Skip `*pp` past run of spaces / tabs. */
static void skip_ws(const char **pp) {
    const char *p = *pp;
    while (*p == ' ' || *p == '\t') p++;
    *pp = p;
}

/* Copy next whitespace-delimited token into `out` (size `cap`),
   advance `*pp` past it. Returns token length (0 if none). */
static int next_tok(const char **pp, char *out, int cap) {
    skip_ws(pp);
    const char *p = *pp;
    int n = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n' && n < cap - 1) {
        out[n++] = *p++;
    }
    out[n] = '\0';
    *pp = p;
    return n;
}

/* Process one parsed entry. Returns 1 on successful mount, 0 on
   skip/error (logged). */
static int apply_entry(const char *source, const char *mountpoint,
                       const char *type, const char *flags);

int fstab_do_mount(const char *source, const char *mountpoint,
                   const char *type, const char *flags) {
    if (!source || !mountpoint || !type) return -1;
    if (!flags) flags = "rw";
    return apply_entry(source, mountpoint, type, flags) ? 0 : -1;
}

static int apply_entry(const char *source, const char *mountpoint,
                       const char *type, const char *flags) {
    int read_only = (strcmp(flags, "ro") == 0);

    /* procfs is a synthetic fs with no backing blkdev — handle it
       before the blkdev_get() that other fstab types need. The
       `source` token is conventional ("proc") but unused. */
    if (strcmp(type, "proc") == 0 || strcmp(type, "procfs") == 0) {
        vfs_node_t *root = procfs_init();
        if (!root) {
            serial_printf("[fstab] %s: procfs_init failed\n", source);
            return 0;
        }
        vfs_mkdir(mountpoint);
        if (vfs_mount(mountpoint, procfs_get_ops(), root, 0) != 0) {
            serial_printf("[fstab] procfs vfs_mount at %s failed\n", mountpoint);
            return 0;
        }
        serial_printf("[fstab] mounted proc at %s\n", mountpoint);
        return 1;
    }

    blkdev_t *dev = blkdev_get(source);
    if (!dev) {
        serial_printf("[fstab] %s: no such block device, skipping\n", source);
        return 0;
    }

    vfs_node_t *root = NULL;
    vfs_ops_t *ops = NULL;
    const char *fs_descr = type;

    if (strcmp(type, "fat") == 0 || strcmp(type, "fat16") == 0 ||
               strcmp(type, "fat32") == 0) {
        root = fat_mount(dev);
        ops = fat_get_ops();
        fs_descr = "fat";
    } else {
        serial_printf("[fstab] %s: unknown fs type '%s'\n", source, type);
        return 0;
    }

    if (!root) {
        serial_printf("[fstab] %s: failed to mount as %s\n", source, type);
        return 0;
    }

    /* The mountpoint directory must exist. mkdir is idempotent here. */
    vfs_mkdir(mountpoint);
    if (vfs_mount(mountpoint, ops, root, root->private_data) != 0) {
        serial_printf("[fstab] %s: vfs_mount at %s failed\n", source, mountpoint);
        return 0;
    }

    strncpy(dev->mount_path, mountpoint, sizeof dev->mount_path - 1);
    dev->mount_path[sizeof dev->mount_path - 1] = '\0';
    strncpy(dev->fs_type, fs_descr, sizeof dev->fs_type - 1);
    dev->fs_type[sizeof dev->fs_type - 1] = '\0';
    dev->read_only = read_only ? 1 : 0;

    serial_printf("[fstab] mounted %s at %s (%s, %s)\n",
                  source, mountpoint, fs_descr, read_only ? "ro" : "rw");
    return 1;
}

int fstab_mount_string(const char *content) {
    if (!content) return 0;
    int mounted = 0;
    const char *p = content;
    while (*p) {
        const char *line_start = p;
        while (*p && *p != '\n') p++;
        int line_len = (int)(p - line_start);
        if (*p == '\n') p++;

        const char *q = line_start;
        while ((q - line_start) < line_len && (*q == ' ' || *q == '\t')) q++;
        if ((q - line_start) >= line_len || *q == '#' || *q == '\n') continue;

        char source[32], mountpoint[VFS_MAX_PATH], type[16], flags[8];
        if (next_tok(&q, source,     sizeof source)     == 0) continue;
        if (next_tok(&q, mountpoint, sizeof mountpoint) == 0) continue;
        if (next_tok(&q, type,       sizeof type)       == 0) continue;
        if (next_tok(&q, flags,      sizeof flags)      == 0) {
            strcpy(flags, "rw");
        }

        if (apply_entry(source, mountpoint, type, flags)) mounted++;
    }
    return mounted;
}

int fstab_mount_file(const char *path) {
    struct vfs_stat st;
    if (vfs_stat(path, &st) != 0 || st.type != VFS_FILE) return 0;
    if (st.size == 0 || st.size > 4096) {
        serial_printf("[fstab] %s: size %u unreasonable, skipping\n", path, st.size);
        return 0;
    }

    char *buf = kmalloc(st.size + 1);
    if (!buf) return 0;
    ssize_t n = vfs_read(path, buf, st.size, 0);
    if (n < 0) { kfree(buf); return 0; }
    buf[n] = '\0';

    int mounted = fstab_mount_string(buf);
    kfree(buf);
    return mounted;
}

int fstab_mount_defaults(void) {
    /* Self-contained bootdisk boots don't ship a ramfs /etc/fstab
       because there are no multiboot modules. Default to mounting the
       install partition straight at '/', so the installed system has
       no '/disk' indirection or secondary chroot layer. The initial
       ramfs at '/' (mounted unconditionally during VFS setup) is
       detached by vfs_mount's replace-on-collision behavior. */
    static const char *DEFAULT_FSTAB =
        "ata0p0 / fat rw\n"
        "proc /proc proc rw\n";
    serial_printf("[fstab] using built-in defaults (no /etc/fstab)\n");
    return fstab_mount_string(DEFAULT_FSTAB);
}
