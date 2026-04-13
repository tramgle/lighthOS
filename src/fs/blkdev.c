#include "fs/blkdev.h"
#include "lib/string.h"
#include "lib/kprintf.h"

static blkdev_t *devices[MAX_BLKDEVS];
static int device_count = 0;

void blkdev_register(blkdev_t *dev) {
    if (device_count >= MAX_BLKDEVS) {
        kprintf("blkdev: max devices reached\n");
        return;
    }
    devices[device_count++] = dev;
    serial_printf("[blkdev] Registered '%s' (%u sectors)\n",
                  dev->name, dev->total_sectors);
}

blkdev_t *blkdev_get(const char *name) {
    for (int i = 0; i < device_count; i++) {
        if (strcmp(devices[i]->name, name) == 0) {
            return devices[i];
        }
    }
    return NULL;
}
