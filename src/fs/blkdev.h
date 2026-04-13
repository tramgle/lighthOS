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
} blkdev_t;

void      blkdev_register(blkdev_t *dev);
blkdev_t *blkdev_get(const char *name);

#endif
