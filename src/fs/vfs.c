#include "fs/vfs.h"
#include "lib/string.h"
#include "lib/kprintf.h"

static vfs_mount_t mounts[MAX_MOUNTS];

void vfs_init(void) {
    memset(mounts, 0, sizeof(mounts));
}

int vfs_mount(const char *path, vfs_ops_t *ops, vfs_node_t *root, void *fs_data) {
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (!mounts[i].in_use) {
            strncpy(mounts[i].mount_point, path, VFS_MAX_PATH - 1);
            mounts[i].ops     = ops;
            mounts[i].root    = root;
            mounts[i].fs_data = fs_data;
            mounts[i].in_use  = true;
            root->mount = &mounts[i];
            serial_printf("[vfs] Mounted at '%s'\n", path);
            return 0;
        }
    }
    return -1;
}

int vfs_umount(const char *path) {
    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (mounts[i].in_use && strcmp(mounts[i].mount_point, path) == 0) {
            mounts[i].in_use = false;
            serial_printf("[vfs] Unmounted '%s'\n", path);
            return 0;
        }
    }
    return -1;
}

/* Find the longest matching mount point for a path.
   Returns the mount and sets remainder to the path after the mount point. */
static vfs_mount_t *find_mount(const char *path, const char **remainder) {
    vfs_mount_t *best = NULL;
    size_t best_len = 0;

    for (int i = 0; i < MAX_MOUNTS; i++) {
        if (!mounts[i].in_use) continue;
        size_t mlen = strlen(mounts[i].mount_point);

        /* Check if path starts with this mount point */
        if (strncmp(path, mounts[i].mount_point, mlen) == 0) {
            /* Must match at a path boundary */
            if (mlen == 1 || path[mlen] == '/' || path[mlen] == '\0') {
                if (mlen > best_len) {
                    best = &mounts[i];
                    best_len = mlen;
                }
            }
        }
    }

    if (best && remainder) {
        *remainder = path + best_len;
        if (**remainder == '/') (*remainder)++;
        if (**remainder == '\0') *remainder = NULL;
    }
    return best;
}

vfs_node_t *vfs_resolve(const char *path) {
    const char *rem = NULL;
    vfs_mount_t *mnt = find_mount(path, &rem);
    if (!mnt) return NULL;

    vfs_node_t *node = mnt->root;
    if (!rem) return node;

    /* Walk path components */
    char component[VFS_MAX_NAME];
    while (rem && *rem) {
        const char *slash = strchr(rem, '/');
        size_t len;
        if (slash) {
            len = slash - rem;
        } else {
            len = strlen(rem);
        }
        if (len == 0) { rem = slash ? slash + 1 : NULL; continue; }
        if (len >= VFS_MAX_NAME) return NULL;

        memcpy(component, rem, len);
        component[len] = '\0';

        if (!node->ops || !node->ops->finddir) return NULL;
        node = node->ops->finddir(node, component);
        if (!node) return NULL;

        rem = slash ? slash + 1 : NULL;
    }
    return node;
}

/* Split a path into parent dir path and filename.
   e.g., "/foo/bar.txt" -> parent="/foo", name="bar.txt"
   Returns the parent node and sets name_out. */
static vfs_node_t *resolve_parent(const char *path, const char **name_out) {
    /* Find last slash */
    const char *last_slash = NULL;
    for (const char *p = path; *p; p++) {
        if (*p == '/') last_slash = p;
    }
    if (!last_slash) return NULL;

    if (last_slash == path) {
        /* Parent is root */
        *name_out = path + 1;
        return vfs_resolve("/");
    }

    char parent_path[VFS_MAX_PATH];
    size_t plen = last_slash - path;
    if (plen >= VFS_MAX_PATH) return NULL;
    memcpy(parent_path, path, plen);
    parent_path[plen] = '\0';

    *name_out = last_slash + 1;
    return vfs_resolve(parent_path);
}

ssize_t vfs_read(const char *path, void *buf, size_t size, off_t offset) {
    vfs_node_t *node = vfs_resolve(path);
    if (!node || !node->ops || !node->ops->read) return -1;
    return node->ops->read(node, buf, size, offset);
}

ssize_t vfs_write(const char *path, const void *buf, size_t size, off_t offset) {
    vfs_node_t *node = vfs_resolve(path);
    if (!node || !node->ops || !node->ops->write) return -1;
    return node->ops->write(node, buf, size, offset);
}

int vfs_readdir(const char *path, uint32_t index, char *name, uint32_t *type) {
    vfs_node_t *node = vfs_resolve(path);
    if (!node || !node->ops || !node->ops->readdir) return -1;
    return node->ops->readdir(node, index, name, type);
}

int vfs_create(const char *path, uint32_t type) {
    const char *name;
    vfs_node_t *parent = resolve_parent(path, &name);
    if (!parent || !parent->ops || !parent->ops->create) return -1;
    if (strlen(name) == 0) return -1;
    return parent->ops->create(parent, name, type);
}

int vfs_unlink(const char *path) {
    const char *name;
    vfs_node_t *parent = resolve_parent(path, &name);
    if (!parent || !parent->ops || !parent->ops->unlink) return -1;
    return parent->ops->unlink(parent, name);
}

int vfs_stat(const char *path, struct vfs_stat *st) {
    vfs_node_t *node = vfs_resolve(path);
    if (!node || !node->ops || !node->ops->stat) return -1;
    return node->ops->stat(node, st);
}

int vfs_mkdir(const char *path) {
    const char *name;
    vfs_node_t *parent = resolve_parent(path, &name);
    if (!parent || !parent->ops || !parent->ops->mkdir) return -1;
    return parent->ops->mkdir(parent, name);
}
