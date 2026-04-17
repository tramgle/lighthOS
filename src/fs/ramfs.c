#include "fs/ramfs.h"
#include "mm/heap.h"
#include "lib/string.h"
#include "lib/kprintf.h"

#define RAMFS_MAX_CHILDREN 256

typedef struct ramfs_inode {
    char     name[VFS_MAX_NAME];
    uint32_t type;
    uint32_t inode_num;
    /* File data */
    uint8_t *data;
    uint32_t size;
    uint32_t capacity;
    /* Directory children */
    struct ramfs_inode *children[RAMFS_MAX_CHILDREN];
    uint32_t child_count;
    struct ramfs_inode *parent;
} ramfs_inode_t;

static uint32_t next_inode = 0;
static vfs_ops_t ramfs_ops;

static ramfs_inode_t *ramfs_alloc_inode(const char *name, uint32_t type) {
    ramfs_inode_t *inode = kmalloc(sizeof(ramfs_inode_t));
    if (!inode) return NULL;
    memset(inode, 0, sizeof(ramfs_inode_t));
    strncpy(inode->name, name, VFS_MAX_NAME - 1);
    inode->type = type;
    inode->inode_num = next_inode++;
    return inode;
}

static vfs_node_t *inode_to_vfs_node(ramfs_inode_t *inode) {
    if (!inode) return NULL;
    vfs_node_t *node = kmalloc(sizeof(vfs_node_t));
    if (!node) return NULL;
    memset(node, 0, sizeof(vfs_node_t));
    strncpy(node->name, inode->name, VFS_MAX_NAME - 1);
    node->type         = inode->type;
    node->size         = inode->size;
    node->inode        = inode->inode_num;
    node->ops          = &ramfs_ops;
    node->private_data = inode;
    return node;
}

static int ramfs_open(vfs_node_t *node, uint32_t flags) {
    (void)node; (void)flags;
    return 0;
}

static int ramfs_close(vfs_node_t *node) {
    (void)node;
    return 0;
}

/* Cap ramfs files at 4 GiB since the inode's size/capacity fields are
 * uint32_t. A user with lseek + write could otherwise push `offset +
 * size` past 2^32, wrap `needed`, and make memcpy scribble kernel
 * memory. Enforce it explicitly both on read (range clamp) and on
 * write (refuse before allocating). */
#define RAMFS_MAX_FILE 0xFFFFFFFFu

static ssize_t ramfs_read(vfs_node_t *node, void *buf, size_t size, off_t offset) {
    ramfs_inode_t *inode = (ramfs_inode_t *)node->private_data;
    if (inode->type != VFS_FILE) return -1;
    if (offset < 0) return -1;
    if ((uint64_t)offset >= inode->size) return 0;
    uint64_t avail = inode->size - (uint64_t)offset;
    if (size > avail) size = (size_t)avail;
    memcpy(buf, inode->data + offset, size);
    return (ssize_t)size;
}

static ssize_t ramfs_write(vfs_node_t *node, const void *buf, size_t size, off_t offset) {
    ramfs_inode_t *inode = (ramfs_inode_t *)node->private_data;
    if (inode->type != VFS_FILE) return -1;
    if (offset < 0) return -1;

    /* Promote to 64-bit so the range check can reject oversized
       writes before the uint32_t truncation. RAMFS_MAX_FILE caps at
       4 GiB - 1 to keep size/capacity representable. */
    uint64_t end = (uint64_t)offset + (uint64_t)size;
    if (end < (uint64_t)offset || end > RAMFS_MAX_FILE) return -1;
    uint32_t needed = (uint32_t)end;

    if (needed > inode->capacity) {
        /* Double on grow, but cap the new capacity at RAMFS_MAX_FILE
           so a user asking for exactly the max doesn't try to
           allocate twice that. */
        uint64_t new_cap64 = (uint64_t)needed * 2;
        if (new_cap64 > RAMFS_MAX_FILE) new_cap64 = RAMFS_MAX_FILE;
        if (new_cap64 < 256) new_cap64 = 256;
        uint32_t new_cap = (uint32_t)new_cap64;
        uint8_t *new_data = kmalloc(new_cap);
        if (!new_data) return -1;
        if (inode->data) {
            memcpy(new_data, inode->data, inode->size);
            kfree(inode->data);
        }
        inode->data = new_data;
        inode->capacity = new_cap;
    }

    memcpy(inode->data + offset, buf, size);
    if (needed > inode->size) inode->size = needed;
    node->size = inode->size;
    return (ssize_t)size;
}

static int ramfs_readdir(vfs_node_t *dir, uint32_t index, char *name_out, uint32_t *type_out) {
    ramfs_inode_t *inode = (ramfs_inode_t *)dir->private_data;
    if (inode->type != VFS_DIR) return -1;
    if (index >= inode->child_count) return -1;
    strncpy(name_out, inode->children[index]->name, VFS_MAX_NAME - 1);
    *type_out = inode->children[index]->type;
    return 0;
}

static vfs_node_t *ramfs_finddir(vfs_node_t *dir, const char *name) {
    ramfs_inode_t *inode = (ramfs_inode_t *)dir->private_data;
    if (inode->type != VFS_DIR) return NULL;
    for (uint32_t i = 0; i < inode->child_count; i++) {
        if (strcmp(inode->children[i]->name, name) == 0) {
            return inode_to_vfs_node(inode->children[i]);
        }
    }
    return NULL;
}

static int ramfs_create(vfs_node_t *dir, const char *name, uint32_t type) {
    ramfs_inode_t *parent = (ramfs_inode_t *)dir->private_data;
    if (parent->type != VFS_DIR) return -1;
    if (parent->child_count >= RAMFS_MAX_CHILDREN) return -1;

    /* Check for duplicate */
    for (uint32_t i = 0; i < parent->child_count; i++) {
        if (strcmp(parent->children[i]->name, name) == 0) return -1;
    }

    ramfs_inode_t *child = ramfs_alloc_inode(name, type);
    if (!child) return -1;
    child->parent = parent;
    parent->children[parent->child_count++] = child;
    return 0;
}

static int ramfs_unlink(vfs_node_t *dir, const char *name) {
    ramfs_inode_t *parent = (ramfs_inode_t *)dir->private_data;
    if (parent->type != VFS_DIR) return -1;

    for (uint32_t i = 0; i < parent->child_count; i++) {
        if (strcmp(parent->children[i]->name, name) == 0) {
            ramfs_inode_t *victim = parent->children[i];
            /* Don't allow deleting non-empty directories */
            if (victim->type == VFS_DIR && victim->child_count > 0) return -1;
            if (victim->data) kfree(victim->data);
            kfree(victim);
            /* Shift remaining children */
            for (uint32_t j = i; j < parent->child_count - 1; j++) {
                parent->children[j] = parent->children[j + 1];
            }
            parent->child_count--;
            return 0;
        }
    }
    return -1;
}

static int ramfs_stat(vfs_node_t *node, struct vfs_stat *st) {
    ramfs_inode_t *inode = (ramfs_inode_t *)node->private_data;
    st->inode = inode->inode_num;
    st->type  = inode->type;
    st->size  = inode->size;
    return 0;
}

static int ramfs_mkdir(vfs_node_t *dir, const char *name) {
    return ramfs_create(dir, name, VFS_DIR);
}

static vfs_ops_t ramfs_ops = {
    .open    = ramfs_open,
    .close   = ramfs_close,
    .read    = ramfs_read,
    .write   = ramfs_write,
    .readdir = ramfs_readdir,
    .finddir = ramfs_finddir,
    .create  = ramfs_create,
    .unlink  = ramfs_unlink,
    .stat    = ramfs_stat,
    .mkdir   = ramfs_mkdir,
};

vfs_ops_t *ramfs_get_ops(void) {
    return &ramfs_ops;
}

vfs_node_t *ramfs_init(void) {
    ramfs_inode_t *root_inode = ramfs_alloc_inode("/", VFS_DIR);
    if (!root_inode) return NULL;
    return inode_to_vfs_node(root_inode);
}
