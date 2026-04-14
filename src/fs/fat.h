#ifndef FAT_H
#define FAT_H

#include "fs/vfs.h"
#include "fs/blkdev.h"

/* Read-only FAT16 driver. Mounts an MBR/BPB-style partition exposed
   as a blkdev (typically created via `blkdev_partition` in main.c).
   Directory enumeration and file reads only — no writes yet. */
vfs_node_t *fat_mount(blkdev_t *dev);
vfs_ops_t  *fat_get_ops(void);

#endif
