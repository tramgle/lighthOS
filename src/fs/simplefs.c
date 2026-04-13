#include "fs/simplefs.h"
#include "mm/heap.h"
#include "lib/string.h"
#include "lib/kprintf.h"

/* In-memory state for a mounted simplefs */
typedef struct {
    blkdev_t        *dev;
    simplefs_super_t super;
    uint8_t          bitmap[SFS_BITMAP_SECTS * BLOCK_SIZE];
    simplefs_inode_t inodes[SFS_MAX_INODES];
} sfs_state_t;

static vfs_ops_t sfs_ops;

/* ---- Low-level helpers ---- */

static int sfs_read_super(sfs_state_t *st) {
    uint8_t buf[BLOCK_SIZE];
    if (st->dev->read_sector(st->dev, 0, buf)) return -1;
    memcpy(&st->super, buf, sizeof(simplefs_super_t));
    return 0;
}

static int sfs_read_bitmap(sfs_state_t *st) {
    for (int i = 0; i < SFS_BITMAP_SECTS; i++) {
        if (st->dev->read_sector(st->dev, SFS_BITMAP_START + i,
                                  st->bitmap + i * BLOCK_SIZE))
            return -1;
    }
    return 0;
}

static int sfs_write_bitmap(sfs_state_t *st) {
    for (int i = 0; i < SFS_BITMAP_SECTS; i++) {
        if (st->dev->write_sector(st->dev, SFS_BITMAP_START + i,
                                   st->bitmap + i * BLOCK_SIZE))
            return -1;
    }
    return 0;
}

static int sfs_read_inodes(sfs_state_t *st) {
    uint8_t *p = (uint8_t *)st->inodes;
    for (int i = 0; i < SFS_INODE_SECTS; i++) {
        if (st->dev->read_sector(st->dev, SFS_INODE_START + i,
                                  p + i * BLOCK_SIZE))
            return -1;
    }
    return 0;
}

static int sfs_write_inode(sfs_state_t *st, uint32_t ino) {
    /* Each sector holds 2 inodes (512/256) */
    uint32_t sect = SFS_INODE_START + ino / 2;
    uint8_t buf[BLOCK_SIZE];
    /* Read the sector, update the inode, write back */
    if (st->dev->read_sector(st->dev, sect, buf)) return -1;
    uint32_t offset = (ino % 2) * SFS_INODE_SIZE;
    memcpy(buf + offset, &st->inodes[ino], SFS_INODE_SIZE);
    return st->dev->write_sector(st->dev, sect, buf);
}

/* Bitmap operations for data block allocation */
static bool sfs_bitmap_test(sfs_state_t *st, uint32_t block) {
    return st->bitmap[block / 8] & (1 << (block % 8));
}

static void sfs_bitmap_set(sfs_state_t *st, uint32_t block) {
    st->bitmap[block / 8] |= (1 << (block % 8));
}

static void sfs_bitmap_clear(sfs_state_t *st, uint32_t block) {
    st->bitmap[block / 8] &= ~(1 << (block % 8));
}

static int32_t sfs_alloc_block(sfs_state_t *st) {
    uint32_t max_blocks = st->super.total_blocks - SFS_DATA_START;
    for (uint32_t i = 0; i < max_blocks; i++) {
        if (!sfs_bitmap_test(st, i)) {
            sfs_bitmap_set(st, i);
            return (int32_t)i;
        }
    }
    return -1;
}

static int32_t sfs_alloc_inode(sfs_state_t *st) {
    for (uint32_t i = 0; i < SFS_MAX_INODES; i++) {
        if (st->inodes[i].type == 0) return (int32_t)i;
    }
    return -1;
}

/* ---- VFS operations ---- */

static sfs_state_t *node_state(vfs_node_t *node) {
    return (sfs_state_t *)node->mount->fs_data;
}

static simplefs_inode_t *node_inode(vfs_node_t *node) {
    sfs_state_t *st = node_state(node);
    return &st->inodes[node->inode];
}

static int sfs_open(vfs_node_t *node, uint32_t flags) {
    (void)node; (void)flags;
    return 0;
}

static int sfs_close(vfs_node_t *node) {
    (void)node;
    return 0;
}

static ssize_t sfs_read(vfs_node_t *node, void *buf, size_t size, off_t offset) {
    sfs_state_t *st = node_state(node);
    simplefs_inode_t *ino = node_inode(node);
    if (ino->type != VFS_FILE) return -1;
    if (offset >= ino->size) return 0;
    if (offset + size > ino->size) size = ino->size - offset;

    uint8_t sector_buf[BLOCK_SIZE];
    size_t bytes_read = 0;

    while (bytes_read < size) {
        uint32_t file_pos = offset + bytes_read;
        uint32_t block_idx = file_pos / BLOCK_SIZE;
        uint32_t block_off = file_pos % BLOCK_SIZE;

        if (block_idx >= ino->block_count) break;
        uint32_t sector = SFS_DATA_START + ino->blocks[block_idx];

        if (st->dev->read_sector(st->dev, sector, sector_buf)) return -1;

        uint32_t chunk = BLOCK_SIZE - block_off;
        if (chunk > size - bytes_read) chunk = size - bytes_read;
        memcpy((uint8_t *)buf + bytes_read, sector_buf + block_off, chunk);
        bytes_read += chunk;
    }
    return (ssize_t)bytes_read;
}

static ssize_t sfs_write(vfs_node_t *node, const void *buf, size_t size, off_t offset) {
    sfs_state_t *st = node_state(node);
    simplefs_inode_t *ino = node_inode(node);
    if (ino->type != VFS_FILE) return -1;

    uint8_t sector_buf[BLOCK_SIZE];
    size_t bytes_written = 0;

    while (bytes_written < size) {
        uint32_t file_pos = offset + bytes_written;
        uint32_t block_idx = file_pos / BLOCK_SIZE;
        uint32_t block_off = file_pos % BLOCK_SIZE;

        /* Allocate new blocks if needed */
        while (block_idx >= ino->block_count) {
            if (ino->block_count >= SFS_MAX_BLOCKS) return bytes_written > 0 ? (ssize_t)bytes_written : -1;
            int32_t blk = sfs_alloc_block(st);
            if (blk < 0) return bytes_written > 0 ? (ssize_t)bytes_written : -1;
            ino->blocks[ino->block_count++] = blk;
        }

        uint32_t sector = SFS_DATA_START + ino->blocks[block_idx];

        /* Read-modify-write if partial block */
        if (block_off != 0 || (size - bytes_written) < BLOCK_SIZE) {
            memset(sector_buf, 0, BLOCK_SIZE);
            st->dev->read_sector(st->dev, sector, sector_buf);
        }

        uint32_t chunk = BLOCK_SIZE - block_off;
        if (chunk > size - bytes_written) chunk = size - bytes_written;
        memcpy(sector_buf + block_off, (const uint8_t *)buf + bytes_written, chunk);

        if (st->dev->write_sector(st->dev, sector, sector_buf)) return -1;
        bytes_written += chunk;
    }

    if (offset + size > ino->size) ino->size = offset + size;
    node->size = ino->size;

    sfs_write_inode(st, node->inode);
    sfs_write_bitmap(st);
    return (ssize_t)bytes_written;
}

static int sfs_readdir(vfs_node_t *dir, uint32_t index, char *name_out, uint32_t *type_out) {
    sfs_state_t *st = node_state(dir);
    uint32_t count = 0;
    for (uint32_t i = 0; i < SFS_MAX_INODES; i++) {
        if (st->inodes[i].type != 0 && st->inodes[i].parent_inode == dir->inode && i != dir->inode) {
            if (count == index) {
                strncpy(name_out, st->inodes[i].name, VFS_MAX_NAME - 1);
                *type_out = st->inodes[i].type;
                return 0;
            }
            count++;
        }
    }
    return -1;
}

static vfs_node_t *sfs_finddir(vfs_node_t *dir, const char *name) {
    sfs_state_t *st = node_state(dir);
    for (uint32_t i = 0; i < SFS_MAX_INODES; i++) {
        if (st->inodes[i].type != 0 &&
            st->inodes[i].parent_inode == dir->inode &&
            i != dir->inode &&
            strcmp(st->inodes[i].name, name) == 0) {
            vfs_node_t *node = kmalloc(sizeof(vfs_node_t));
            if (!node) return NULL;
            memset(node, 0, sizeof(vfs_node_t));
            strncpy(node->name, st->inodes[i].name, VFS_MAX_NAME - 1);
            node->type         = st->inodes[i].type;
            node->size         = st->inodes[i].size;
            node->inode        = i;
            node->ops          = &sfs_ops;
            node->mount        = dir->mount;
            node->private_data = NULL;
            return node;
        }
    }
    return NULL;
}

static int sfs_create(vfs_node_t *dir, const char *name, uint32_t type) {
    sfs_state_t *st = node_state(dir);

    /* Check duplicate */
    for (uint32_t i = 0; i < SFS_MAX_INODES; i++) {
        if (st->inodes[i].type != 0 &&
            st->inodes[i].parent_inode == dir->inode &&
            strcmp(st->inodes[i].name, name) == 0) {
            return -1;
        }
    }

    int32_t ino = sfs_alloc_inode(st);
    if (ino < 0) return -1;

    memset(&st->inodes[ino], 0, SFS_INODE_SIZE);
    st->inodes[ino].inode_num    = ino;
    st->inodes[ino].type         = type;
    st->inodes[ino].parent_inode = dir->inode;
    strncpy(st->inodes[ino].name, name, SFS_NAME_LEN - 1);

    sfs_write_inode(st, ino);
    return 0;
}

static int sfs_unlink(vfs_node_t *dir, const char *name) {
    sfs_state_t *st = node_state(dir);
    for (uint32_t i = 0; i < SFS_MAX_INODES; i++) {
        if (st->inodes[i].type != 0 &&
            st->inodes[i].parent_inode == dir->inode &&
            strcmp(st->inodes[i].name, name) == 0) {
            /* Free data blocks */
            for (uint32_t b = 0; b < st->inodes[i].block_count; b++) {
                sfs_bitmap_clear(st, st->inodes[i].blocks[b]);
            }
            memset(&st->inodes[i], 0, SFS_INODE_SIZE);
            sfs_write_inode(st, i);
            sfs_write_bitmap(st);
            return 0;
        }
    }
    return -1;
}

static int sfs_stat(vfs_node_t *node, struct vfs_stat *st_out) {
    simplefs_inode_t *ino = node_inode(node);
    st_out->inode = ino->inode_num;
    st_out->type  = ino->type;
    st_out->size  = ino->size;
    return 0;
}

static int sfs_mkdir(vfs_node_t *dir, const char *name) {
    return sfs_create(dir, name, VFS_DIR);
}

static vfs_ops_t sfs_ops = {
    .open    = sfs_open,
    .close   = sfs_close,
    .read    = sfs_read,
    .write   = sfs_write,
    .readdir = sfs_readdir,
    .finddir = sfs_finddir,
    .create  = sfs_create,
    .unlink  = sfs_unlink,
    .stat    = sfs_stat,
    .mkdir   = sfs_mkdir,
};

/* ---- Public API ---- */

int simplefs_format(blkdev_t *dev) {
    uint8_t buf[BLOCK_SIZE];

    /* Write superblock */
    simplefs_super_t sb;
    memset(&sb, 0, sizeof(sb));
    sb.magic        = SFS_MAGIC;
    sb.version      = SFS_VERSION;
    sb.total_blocks = dev->total_sectors;
    sb.inode_count  = SFS_MAX_INODES;
    sb.data_start   = SFS_DATA_START;
    sb.bitmap_start = SFS_BITMAP_START;
    sb.root_inode   = 0;
    memset(buf, 0, BLOCK_SIZE);
    memcpy(buf, &sb, sizeof(sb));
    if (dev->write_sector(dev, 0, buf)) return -1;

    /* Clear bitmap */
    memset(buf, 0, BLOCK_SIZE);
    for (int i = 0; i < SFS_BITMAP_SECTS; i++) {
        if (dev->write_sector(dev, SFS_BITMAP_START + i, buf)) return -1;
    }

    /* Clear inode table */
    for (int i = 0; i < SFS_INODE_SECTS; i++) {
        if (dev->write_sector(dev, SFS_INODE_START + i, buf)) return -1;
    }

    /* Create root inode (inode 0) */
    simplefs_inode_t root;
    memset(&root, 0, SFS_INODE_SIZE);
    root.inode_num    = 0;
    root.type         = VFS_DIR;
    root.parent_inode = 0;
    strncpy(root.name, "/", SFS_NAME_LEN);

    memset(buf, 0, BLOCK_SIZE);
    memcpy(buf, &root, SFS_INODE_SIZE);
    if (dev->write_sector(dev, SFS_INODE_START, buf)) return -1;

    serial_printf("[simplefs] Formatted device '%s'\n", dev->name);
    return 0;
}

vfs_node_t *simplefs_mount(blkdev_t *dev) {
    sfs_state_t *st = kmalloc(sizeof(sfs_state_t));
    if (!st) return NULL;
    memset(st, 0, sizeof(sfs_state_t));
    st->dev = dev;

    if (sfs_read_super(st)) { kfree(st); return NULL; }
    if (st->super.magic != SFS_MAGIC) {
        serial_printf("[simplefs] Bad magic on '%s', formatting...\n", dev->name);
        if (simplefs_format(dev)) { kfree(st); return NULL; }
        if (sfs_read_super(st)) { kfree(st); return NULL; }
    }

    if (sfs_read_bitmap(st)) { kfree(st); return NULL; }
    if (sfs_read_inodes(st)) { kfree(st); return NULL; }

    /* Create VFS node for root */
    vfs_node_t *root = kmalloc(sizeof(vfs_node_t));
    if (!root) { kfree(st); return NULL; }
    memset(root, 0, sizeof(vfs_node_t));
    strncpy(root->name, "/", VFS_MAX_NAME);
    root->type         = VFS_DIR;
    root->inode        = st->super.root_inode;
    root->ops          = &sfs_ops;
    root->private_data = st;

    serial_printf("[simplefs] Mounted '%s'\n", dev->name);
    return root;
}

vfs_ops_t *simplefs_get_ops(void) {
    return &sfs_ops;
}
