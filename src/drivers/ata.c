#include "drivers/ata.h"
#include "include/io.h"
#include "lib/string.h"
#include "lib/kprintf.h"

#define ATA_PRIMARY_DATA    0x1F0
#define ATA_PRIMARY_ERR     0x1F1
#define ATA_PRIMARY_COUNT   0x1F2
#define ATA_PRIMARY_LBA_LO  0x1F3
#define ATA_PRIMARY_LBA_MI  0x1F4
#define ATA_PRIMARY_LBA_HI  0x1F5
#define ATA_PRIMARY_DRIVE   0x1F6
#define ATA_PRIMARY_CMD     0x1F7
#define ATA_PRIMARY_STATUS  0x1F7
#define ATA_PRIMARY_CTRL    0x3F6

#define ATA_STATUS_BSY  0x80
#define ATA_STATUS_DRDY 0x40
#define ATA_STATUS_DRQ  0x08
#define ATA_STATUS_ERR  0x01

#define ATA_CMD_READ    0x20
#define ATA_CMD_WRITE   0x30
#define ATA_CMD_FLUSH   0xE7
#define ATA_CMD_IDENTIFY 0xEC

static blkdev_t ata_dev;
static bool ata_present = false;

static void ata_400ns_delay(void) {
    inb(ATA_PRIMARY_STATUS);
    inb(ATA_PRIMARY_STATUS);
    inb(ATA_PRIMARY_STATUS);
    inb(ATA_PRIMARY_STATUS);
}

static int ata_wait_bsy(void) {
    for (int i = 0; i < 100000; i++) {
        uint8_t status = inb(ATA_PRIMARY_STATUS);
        if (!(status & ATA_STATUS_BSY)) return 0;
    }
    return -1;
}

static int ata_wait_drq(void) {
    for (int i = 0; i < 100000; i++) {
        uint8_t status = inb(ATA_PRIMARY_STATUS);
        if (status & ATA_STATUS_ERR) return -1;
        if (status & ATA_STATUS_DRQ) return 0;
    }
    return -1;
}

static int ata_read_sector(blkdev_t *dev, uint32_t lba, void *buf) {
    (void)dev;
    if (ata_wait_bsy()) return -1;

    outb(ATA_PRIMARY_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));
    outb(ATA_PRIMARY_COUNT, 1);
    outb(ATA_PRIMARY_LBA_LO, lba & 0xFF);
    outb(ATA_PRIMARY_LBA_MI, (lba >> 8) & 0xFF);
    outb(ATA_PRIMARY_LBA_HI, (lba >> 16) & 0xFF);
    outb(ATA_PRIMARY_CMD, ATA_CMD_READ);

    ata_400ns_delay();
    if (ata_wait_drq()) return -1;

    uint16_t *wbuf = (uint16_t *)buf;
    for (int i = 0; i < 256; i++) {
        wbuf[i] = inw(ATA_PRIMARY_DATA);
    }
    return 0;
}

static int ata_write_sector(blkdev_t *dev, uint32_t lba, const void *buf) {
    (void)dev;
    if (ata_wait_bsy()) return -1;

    outb(ATA_PRIMARY_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));
    outb(ATA_PRIMARY_COUNT, 1);
    outb(ATA_PRIMARY_LBA_LO, lba & 0xFF);
    outb(ATA_PRIMARY_LBA_MI, (lba >> 8) & 0xFF);
    outb(ATA_PRIMARY_LBA_HI, (lba >> 16) & 0xFF);
    outb(ATA_PRIMARY_CMD, ATA_CMD_WRITE);

    ata_400ns_delay();
    if (ata_wait_drq()) return -1;

    const uint16_t *wbuf = (const uint16_t *)buf;
    for (int i = 0; i < 256; i++) {
        outw(ATA_PRIMARY_DATA, wbuf[i]);
    }

    /* Flush cache. If the flush times out the sector was still sent
       to the drive but may not have hit media yet — surface the
       failure so callers (panic.log, install, etc.) can decide
       whether to retry. */
    outb(ATA_PRIMARY_CMD, ATA_CMD_FLUSH);
    if (ata_wait_bsy() != 0) {
        serial_printf("[ata] flush wait timed out at lba=%u\n", lba);
        return -1;
    }
    return 0;
}

void ata_init(void) {
    /* Select master drive */
    outb(ATA_PRIMARY_DRIVE, 0xA0);
    ata_400ns_delay();

    /* Send IDENTIFY command */
    outb(ATA_PRIMARY_COUNT, 0);
    outb(ATA_PRIMARY_LBA_LO, 0);
    outb(ATA_PRIMARY_LBA_MI, 0);
    outb(ATA_PRIMARY_LBA_HI, 0);
    outb(ATA_PRIMARY_CMD, ATA_CMD_IDENTIFY);

    ata_400ns_delay();
    uint8_t status = inb(ATA_PRIMARY_STATUS);
    if (status == 0) {
        serial_printf("[ata] No drive detected\n");
        return;
    }

    if (ata_wait_bsy()) {
        serial_printf("[ata] Drive busy timeout\n");
        return;
    }

    /* Check for non-ATA */
    if (inb(ATA_PRIMARY_LBA_MI) || inb(ATA_PRIMARY_LBA_HI)) {
        serial_printf("[ata] Not an ATA drive\n");
        return;
    }

    if (ata_wait_drq()) {
        serial_printf("[ata] DRQ timeout or error\n");
        return;
    }

    /* Read identify data */
    uint16_t identify[256];
    for (int i = 0; i < 256; i++) {
        identify[i] = inw(ATA_PRIMARY_DATA);
    }

    uint32_t sectors = identify[60] | ((uint32_t)identify[61] << 16);
    if (sectors == 0) {
        serial_printf("[ata] Drive reports 0 sectors\n");
        return;
    }

    ata_present = true;
    strncpy(ata_dev.name, "ata0", sizeof(ata_dev.name));
    ata_dev.total_sectors = sectors;
    ata_dev.read_sector   = ata_read_sector;
    ata_dev.write_sector  = ata_write_sector;
    ata_dev.private_data  = NULL;

    blkdev_register(&ata_dev);
    serial_printf("[ata] Found drive: %u sectors (%uMB)\n",
                  sectors, (sectors * 512) / (1024 * 1024));
}

blkdev_t *ata_get_device(void) {
    return ata_present ? &ata_dev : NULL;
}
