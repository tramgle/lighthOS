#ifndef ATA_H
#define ATA_H

#include "fs/blkdev.h"

void      ata_init(void);
blkdev_t *ata_get_device(void);

#endif
