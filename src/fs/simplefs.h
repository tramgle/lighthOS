#ifndef SIMPLEFS_H
#define SIMPLEFS_H

#include "fs/vfs.h"
#include "fs/blkdev.h"

#define SFS_MAGIC        0x53494D50  /* "SIMP" */
#define SFS_VERSION      3        /* indirect blocks; inode back to 256B */
#define SFS_MAX_INODES   128
#define SFS_INODE_SIZE   256
#define SFS_BITMAP_START 1
#define SFS_BITMAP_SECTS 4
#define SFS_INODE_START  5
#define SFS_INODE_SECTS  64       /* 128 inodes * 256 / 512 */
#define SFS_DATA_START   69
#define SFS_NAME_LEN     64
#define SFS_DIRECT       41       /* direct pointers per inode */
#define SFS_PTRS_PER_BLK 128      /* 512 / 4 */

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t total_blocks;
    uint32_t inode_count;
    uint32_t data_start;
    uint32_t bitmap_start;
    uint32_t root_inode;
    uint8_t  reserved[488];
} __attribute__((packed)) simplefs_super_t;

/* 256-byte inode: 41 direct + 1 single-indirect + 1 double-indirect.
     direct:            41 * 512 = 20 KB
     single-indirect:  128 * 512 = 64 KB
     double-indirect: 128*128*512 = 8 MB
   Max file size ~= 8.08 MB. */
typedef struct {
    uint32_t inode_num;
    uint32_t type;
    uint32_t size;
    uint32_t block_count;           /* logical blocks used (not counting indirect-index blocks) */
    uint32_t parent_inode;
    char     name[SFS_NAME_LEN];
    uint32_t direct[SFS_DIRECT];
    uint32_t indirect;              /* 0 if none */
    uint32_t double_indirect;       /* 0 if none */
} __attribute__((packed)) simplefs_inode_t;
/* 20 + 64 + 41*4 + 8 = 256 bytes */

int         simplefs_format(blkdev_t *dev);
vfs_node_t *simplefs_mount(blkdev_t *dev);
vfs_ops_t  *simplefs_get_ops(void);

#endif
