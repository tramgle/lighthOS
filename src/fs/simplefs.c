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
    /* With SFS_INODE_SIZE=256, two inodes share a 512-byte sector. */
    uint32_t inodes_per_sect = BLOCK_SIZE / SFS_INODE_SIZE;
    uint32_t sect = SFS_INODE_START + ino / inodes_per_sect;
    uint8_t buf[BLOCK_SIZE];
    if (st->dev->read_sector(st->dev, sect, buf)) return -1;
    uint32_t offset = (ino % inodes_per_sect) * SFS_INODE_SIZE;
    memcpy(buf + offset, &st->inodes[ino], SFS_INODE_SIZE);
    return st->dev->write_sector(st->dev, sect, buf);
}

/* Bitmap capacity in bits = bytes × 8. Any block index beyond that
   would walk off the in-memory bitmap and corrupt adjacent state. */
#define SFS_BITMAP_BITS (SFS_BITMAP_SECTS * BLOCK_SIZE * 8)

/* Returns 1 if `block` is a valid data-region block index for the
   mounted fs. Used to vet every block pointer read off disk before
   we follow it. */
static int sfs_data_ok(sfs_state_t *st, uint32_t block) {
    if (block >= SFS_BITMAP_BITS) return 0;
    if (st->super.total_blocks <= SFS_DATA_START) return 0;
    if (block >= st->super.total_blocks - SFS_DATA_START) return 0;
    return 1;
}

/* Bitmap operations for data block allocation. Silently reject
   out-of-range indices so corrupt on-disk pointers can't scribble
   the bitmap. */
static bool sfs_bitmap_test(sfs_state_t *st, uint32_t block) {
    if (block >= SFS_BITMAP_BITS) return true;   /* treat as "in use" */
    return st->bitmap[block / 8] & (1 << (block % 8));
}

static void sfs_bitmap_set(sfs_state_t *st, uint32_t block) {
    if (block >= SFS_BITMAP_BITS) return;
    st->bitmap[block / 8] |= (1 << (block % 8));
}

static void sfs_bitmap_clear(sfs_state_t *st, uint32_t block) {
    if (block >= SFS_BITMAP_BITS) return;
    st->bitmap[block / 8] &= ~(1 << (block % 8));
}

static int32_t sfs_alloc_block(sfs_state_t *st) {
    /* Underflow-guard: total_blocks must exceed the reserved prefix. */
    if (st->super.total_blocks <= SFS_DATA_START) return -1;
    uint32_t max_blocks = st->super.total_blocks - SFS_DATA_START;
    if (max_blocks > SFS_BITMAP_BITS) max_blocks = SFS_BITMAP_BITS;
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

/* Map a logical block index in the file to its disk block number
   (not yet shifted by SFS_DATA_START — caller adds it). `alloc` = 1
   to allocate intermediate indirect blocks and the data block itself
   if they don't exist. Returns 0 (never a valid data-block index here
   because block 0 is the first data block) is avoided by shifting —
   we return BLOCK_SIZE-unit block numbers within the data region,
   where 0 is valid; a negative return indicates failure. */
static int32_t sfs_get_block(sfs_state_t *st, simplefs_inode_t *ino,
                             uint32_t idx, int alloc) {
    /* Direct range. */
    if (idx < SFS_DIRECT) {
        if (ino->direct[idx] == 0 && alloc) {
            int32_t blk = sfs_alloc_block(st);
            if (blk < 0) return -1;
            ino->direct[idx] = (uint32_t)blk + 1;  /* +1 so 0 means "none" */
        }
        if (ino->direct[idx] == 0) return -1;
        uint32_t b = ino->direct[idx] - 1;
        if (!sfs_data_ok(st, b)) return -1;
        return (int32_t)b;
    }
    idx -= SFS_DIRECT;

    /* Single indirect: 128 pointers. */
    if (idx < SFS_PTRS_PER_BLK) {
        if (ino->indirect == 0) {
            if (!alloc) return -1;
            int32_t blk = sfs_alloc_block(st);
            if (blk < 0) return -1;
            ino->indirect = (uint32_t)blk + 1;
            /* Zero the newly allocated indirect block. */
            uint8_t z[BLOCK_SIZE];
            memset(z, 0, BLOCK_SIZE);
            st->dev->write_sector(st->dev, SFS_DATA_START + blk, z);
        }
        uint32_t ind_b = ino->indirect - 1;
        if (!sfs_data_ok(st, ind_b)) return -1;
        uint32_t iblock_sect = SFS_DATA_START + ind_b;
        uint32_t table[SFS_PTRS_PER_BLK];
        if (st->dev->read_sector(st->dev, iblock_sect, table)) return -1;
        if (table[idx] == 0) {
            if (!alloc) return -1;
            int32_t blk = sfs_alloc_block(st);
            if (blk < 0) return -1;
            table[idx] = (uint32_t)blk + 1;
            if (st->dev->write_sector(st->dev, iblock_sect, table)) return -1;
        }
        uint32_t b = table[idx] - 1;
        if (!sfs_data_ok(st, b)) return -1;
        return (int32_t)b;
    }
    idx -= SFS_PTRS_PER_BLK;

    /* Double indirect. */
    uint32_t di_idx = idx / SFS_PTRS_PER_BLK;
    uint32_t si_idx = idx % SFS_PTRS_PER_BLK;
    if (di_idx >= SFS_PTRS_PER_BLK) return -1;   /* past the max */

    if (ino->double_indirect == 0) {
        if (!alloc) return -1;
        int32_t blk = sfs_alloc_block(st);
        if (blk < 0) return -1;
        ino->double_indirect = (uint32_t)blk + 1;
        uint8_t z[BLOCK_SIZE];
        memset(z, 0, BLOCK_SIZE);
        st->dev->write_sector(st->dev, SFS_DATA_START + blk, z);
    }
    uint32_t di_b = ino->double_indirect - 1;
    if (!sfs_data_ok(st, di_b)) return -1;
    uint32_t di_sect = SFS_DATA_START + di_b;
    uint32_t di_table[SFS_PTRS_PER_BLK];
    if (st->dev->read_sector(st->dev, di_sect, di_table)) return -1;
    if (di_table[di_idx] == 0) {
        if (!alloc) return -1;
        int32_t blk = sfs_alloc_block(st);
        if (blk < 0) return -1;
        di_table[di_idx] = (uint32_t)blk + 1;
        uint8_t z[BLOCK_SIZE];
        memset(z, 0, BLOCK_SIZE);
        st->dev->write_sector(st->dev, SFS_DATA_START + blk, z);
        if (st->dev->write_sector(st->dev, di_sect, di_table)) return -1;
    }
    uint32_t si_b = di_table[di_idx] - 1;
    if (!sfs_data_ok(st, si_b)) return -1;
    uint32_t si_sect = SFS_DATA_START + si_b;
    uint32_t si_table[SFS_PTRS_PER_BLK];
    if (st->dev->read_sector(st->dev, si_sect, si_table)) return -1;
    if (si_table[si_idx] == 0) {
        if (!alloc) return -1;
        int32_t blk = sfs_alloc_block(st);
        if (blk < 0) return -1;
        si_table[si_idx] = (uint32_t)blk + 1;
        if (st->dev->write_sector(st->dev, si_sect, si_table)) return -1;
    }
    uint32_t b = si_table[si_idx] - 1;
    if (!sfs_data_ok(st, b)) return -1;
    return (int32_t)b;
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

        int32_t blk = sfs_get_block(st, ino, block_idx, 0);
        if (blk < 0) break;
        uint32_t sector = SFS_DATA_START + blk;

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

        int32_t blk = sfs_get_block(st, ino, block_idx, 1);
        if (blk < 0) return bytes_written > 0 ? (ssize_t)bytes_written : -1;
        if (block_idx >= ino->block_count) ino->block_count = block_idx + 1;

        uint32_t sector = SFS_DATA_START + blk;

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
        simplefs_inode_t *ino = &st->inodes[i];
        if (ino->type == 0 ||
            ino->parent_inode != dir->inode ||
            strcmp(ino->name, name) != 0) continue;

        /* Walk every logical block and free its data sector via the
           block resolver. Then free the index blocks themselves. */
        for (uint32_t b = 0; b < ino->block_count; b++) {
            int32_t blk = sfs_get_block(st, ino, b, 0);
            if (blk >= 0) sfs_bitmap_clear(st, (uint32_t)blk);
        }
        /* Free double-indirect's second-level indirect blocks. */
        if (ino->double_indirect) {
            uint32_t di_sect = SFS_DATA_START + (ino->double_indirect - 1);
            uint32_t di_table[SFS_PTRS_PER_BLK];
            if (st->dev->read_sector(st->dev, di_sect, di_table) == 0) {
                for (uint32_t k = 0; k < SFS_PTRS_PER_BLK; k++) {
                    if (di_table[k]) sfs_bitmap_clear(st, di_table[k] - 1);
                }
            }
            sfs_bitmap_clear(st, ino->double_indirect - 1);
        }
        if (ino->indirect) sfs_bitmap_clear(st, ino->indirect - 1);

        memset(ino, 0, SFS_INODE_SIZE);
        sfs_write_inode(st, i);
        sfs_write_bitmap(st);
        return 0;
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

    /* If the superblock doesn't look like ours, reformat. That covers
       both uninitialized disks and superblocks corrupted beyond
       recognition. */
    int sane =
        (st->super.magic == SFS_MAGIC) &&
        (st->super.version == SFS_VERSION) &&
        (st->super.total_blocks > SFS_DATA_START) &&
        (st->super.total_blocks <= dev->total_sectors) &&
        (st->super.data_start == SFS_DATA_START) &&
        (st->super.bitmap_start == SFS_BITMAP_START) &&
        (st->super.inode_count == SFS_MAX_INODES);

    if (!sane) {
        serial_printf("[simplefs] super on '%s' failed sanity checks — formatting\n", dev->name);
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
