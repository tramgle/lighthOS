#include "drivers/ramdisk.h"
#include "mm/heap.h"
#include "lib/string.h"

static int ramdisk_read(blkdev_t *dev, uint32_t lba, void *buf) {
    uint8_t *data = (uint8_t *)dev->private_data;
    memcpy(buf, data + lba * BLOCK_SIZE, BLOCK_SIZE);
    return 0;
}

static int ramdisk_write(blkdev_t *dev, uint32_t lba, const void *buf) {
    uint8_t *data = (uint8_t *)dev->private_data;
    memcpy(data + lba * BLOCK_SIZE, buf, BLOCK_SIZE);
    return 0;
}

blkdev_t *ramdisk_create(const char *name, uint32_t size_bytes) {
    uint32_t sectors = size_bytes / BLOCK_SIZE;
    void *data = kmalloc(size_bytes);
    if (!data) return NULL;
    memset(data, 0, size_bytes);

    blkdev_t *dev = kmalloc(sizeof(blkdev_t));
    if (!dev) { kfree(data); return NULL; }

    strncpy(dev->name, name, sizeof(dev->name) - 1);
    dev->name[sizeof(dev->name) - 1] = '\0';
    dev->total_sectors = sectors;
    dev->read_sector   = ramdisk_read;
    dev->write_sector  = ramdisk_write;
    dev->private_data  = data;

    blkdev_register(dev);
    return dev;
}

void ramdisk_destroy(blkdev_t *dev) {
    if (dev) {
        kfree(dev->private_data);
        kfree(dev);
    }
}
