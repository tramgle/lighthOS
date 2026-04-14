#ifndef BLKDEV_H
#define BLKDEV_H

#include "include/types.h"

#define BLOCK_SIZE 512
#define MAX_BLKDEVS 8

typedef struct blkdev {
    char name[32];
    uint32_t total_sectors;
    int (*read_sector)(struct blkdev *dev, uint32_t lba, void *buf);
    int (*write_sector)(struct blkdev *dev, uint32_t lba, const void *buf);
    void *private_data;
    /* Set by filesystem code when the device gets mounted; cleared
       when unmounted. `mount_path[0] == '\0'` means not mounted.
       `fs_type` is a short descriptor like "simplefs" or "fat16"
       for lsblk to display; read_only is a hint the user sees. */
    char mount_path[32];
    char fs_type[16];
    uint8_t read_only;
} blkdev_t;

void      blkdev_register(blkdev_t *dev);
blkdev_t *blkdev_get(const char *name);

/* Enumerate registered block devices. `blkdev_count()` is how many
   are alive; `blkdev_nth(i)` returns the i-th or NULL. Used by the
   lsblk user binary via SYS_BLKDEVS. */
int       blkdev_count(void);
blkdev_t *blkdev_nth(int idx);

/* Create a logical block device that presents a window into `parent`
   starting at sector `start`, `count` sectors long. The new device
   forwards read/write to the parent with the offset applied and is
   registered under `name`. Used to carve a simplefs partition out of
   a disk whose early sectors hold the bootloader/kernel. */
blkdev_t *blkdev_partition(blkdev_t *parent, uint32_t start, uint32_t count,
                           const char *name);

#endif
