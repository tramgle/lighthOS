#ifndef RAMDISK_H
#define RAMDISK_H

#include "fs/blkdev.h"

blkdev_t *ramdisk_create(const char *name, uint32_t size_bytes);
void      ramdisk_destroy(blkdev_t *dev);

#endif
