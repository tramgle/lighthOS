#include "fs/blkdev.h"
#include "mm/heap.h"
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

int blkdev_count(void) {
    return device_count;
}

blkdev_t *blkdev_nth(int idx) {
    if (idx < 0 || idx >= device_count) return NULL;
    return devices[idx];
}

typedef struct {
    blkdev_t *parent;
    uint32_t  start;
} part_priv_t;

static int part_read(blkdev_t *dev, uint32_t lba, void *buf) {
    part_priv_t *p = (part_priv_t *)dev->private_data;
    if (lba >= dev->total_sectors) return -1;
    return p->parent->read_sector(p->parent, p->start + lba, buf);
}

static int part_write(blkdev_t *dev, uint32_t lba, const void *buf) {
    part_priv_t *p = (part_priv_t *)dev->private_data;
    if (lba >= dev->total_sectors) return -1;
    return p->parent->write_sector(p->parent, p->start + lba, buf);
}

blkdev_t *blkdev_partition(blkdev_t *parent, uint32_t start, uint32_t count,
                           const char *name) {
    if (!parent) return NULL;
    blkdev_t *dev = (blkdev_t *)kmalloc(sizeof(blkdev_t));
    part_priv_t *priv = (part_priv_t *)kmalloc(sizeof(part_priv_t));
    if (!dev || !priv) {
        if (dev) kfree(dev);
        if (priv) kfree(priv);
        return NULL;
    }
    memset(dev, 0, sizeof(*dev));
    priv->parent = parent;
    priv->start = start;
    strncpy(dev->name, name, sizeof(dev->name) - 1);
    dev->total_sectors = count;
    dev->read_sector = part_read;
    dev->write_sector = part_write;
    dev->private_data = priv;
    blkdev_register(dev);
    return dev;
}
