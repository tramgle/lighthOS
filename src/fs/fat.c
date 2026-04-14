/* Minimal FAT16 read-only driver.
 *
 * Understands the BIOS Parameter Block at LBA 0 of the supplied
 * partition, enumerates the fixed-size root directory, follows 16-bit
 * cluster chains for file reads. 8.3 short names only — no LFN.
 *
 * Not supported (hit ENOSYS / stub return):
 *   - subdirectories (we mount the root and never descend)
 *   - writes, creates, mkdir, unlink
 *   - FAT12, FAT32, exFAT
 *   - long filenames
 *
 * Good enough for the original goal: let the host populate a FAT
 * partition with `mkfs.fat` + `mcopy` and read those files from inside
 * VibeOS. */

#include "fs/fat.h"
#include "mm/heap.h"
#include "lib/string.h"
#include "lib/kprintf.h"

#define FAT_ATTR_DIR       0x10
#define FAT_ATTR_VOLUME_ID 0x08
#define FAT_EOC_MIN        0xFFF8

typedef struct {
    uint8_t  jmp[3];
    char     oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entries;          /* 16-bit because FAT12/16 fix the root */
    uint16_t total_sectors_16;
    uint8_t  media_type;
    uint16_t sectors_per_fat_16;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    /* FAT12/16 extended BPB */
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_sig;
    uint32_t volume_id;
    char     volume_label[11];
    char     fs_type[8];
} __attribute__((packed)) fat_bpb_t;

typedef struct {
    char     name[11];
    uint8_t  attr;
    uint8_t  reserved;
    uint8_t  create_tenths;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t access_date;
    uint16_t first_cluster_hi;
    uint16_t modify_time;
    uint16_t modify_date;
    uint16_t first_cluster_lo;
    uint32_t size;
} __attribute__((packed)) fat_dirent_t;

typedef struct {
    blkdev_t *dev;
    uint32_t  fat_start_lba;         /* first FAT sector */
    uint32_t  root_dir_lba;          /* first sector of root directory */
    uint32_t  root_dir_sectors;      /* count */
    uint32_t  root_entries;          /* entries in the root */
    uint32_t  data_start_lba;        /* LBA of cluster #2 */
    uint32_t  sectors_per_cluster;
    uint32_t  bytes_per_cluster;
    uint32_t  total_clusters;        /* max valid cluster number */
    uint32_t  part_end_lba;          /* one past the last sector of the partition */
} fat_state_t;

static vfs_ops_t fat_ops;

/* Render an 11-byte FAT 8.3 name into `out` (e.g. "HELLO   TXT" -> "HELLO.TXT"). */
static void fat_name_to_dot(const char name[11], char *out, int out_size) {
    int i = 0, o = 0;
    while (i < 8 && name[i] != ' ' && o < out_size - 1) out[o++] = name[i++];
    int ext_start = 8;
    if (name[ext_start] != ' ') {
        if (o < out_size - 1) out[o++] = '.';
        int j = 0;
        while (j < 3 && name[ext_start + j] != ' ' && o < out_size - 1) {
            out[o++] = name[ext_start + j++];
        }
    }
    out[o] = '\0';
}

/* Convert "hello.txt" / "HELLO.TXT" to the 11-byte FAT form for compare. */
static void fat_name_from_dot(const char *user, char out[11]) {
    /* Fill everything with spaces, then overlay name + extension. */
    for (int i = 0; i < 11; i++) out[i] = ' ';

    int j = 0;
    /* Copy up to 8 name chars, upcased, stopping at '.' or EOS. */
    for (int i = 0; i < 8 && user[j] && user[j] != '.'; i++, j++) {
        char c = user[j];
        if (c >= 'a' && c <= 'z') c -= 32;
        out[i] = c;
    }
    /* Skip the dot if present. */
    if (user[j] == '.') j++;
    /* Copy up to 3 extension chars into slots 8..10. */
    for (int k = 0; k < 3 && user[j]; k++, j++) {
        char c = user[j];
        if (c >= 'a' && c <= 'z') c -= 32;
        out[8 + k] = c;
    }
}

/* Walk the FAT to find the next cluster in a chain. Returns 0 on
   end-of-chain (>= 0xFFF8), on a cluster number that's out of range
   (< 2 or > total_clusters), or on read error — all safe sentinels
   that stop chain walkers cleanly. */
static uint16_t fat_next_cluster(fat_state_t *st, uint16_t cluster) {
    if (cluster < 2) return 0;
    if (cluster > st->total_clusters + 1) return 0;
    uint32_t offset = (uint32_t)cluster * 2;
    uint32_t sect = st->fat_start_lba + (offset / 512);
    uint32_t byte_off = offset % 512;
    uint8_t buf[512];
    if (st->dev->read_sector(st->dev, sect, buf)) return 0;
    uint16_t next = buf[byte_off] | ((uint16_t)buf[byte_off + 1] << 8);
    if (next >= FAT_EOC_MIN) return 0;
    if (next < 2 || next > st->total_clusters + 1) return 0;
    return next;
}

/* Locate a directory entry by name inside the root. On success returns
   0 and fills *out with the entry; -1 if not found. */
static int fat_lookup(fat_state_t *st, const char *name, fat_dirent_t *out) {
    char target[11];
    fat_name_from_dot(name, target);

    uint8_t buf[512];
    for (uint32_t i = 0; i < st->root_dir_sectors; i++) {
        if (st->dev->read_sector(st->dev, st->root_dir_lba + i, buf)) return -1;
        for (int j = 0; j < 512; j += 32) {
            fat_dirent_t *e = (fat_dirent_t *)(buf + j);
            if ((uint8_t)e->name[0] == 0x00) return -1;   /* end of dir */
            if ((uint8_t)e->name[0] == 0xE5) continue;    /* deleted */
            if (e->attr & FAT_ATTR_VOLUME_ID) continue;
            if (e->attr == 0x0F) continue;                /* LFN */
            if (memcmp(e->name, target, 11) == 0) {
                memcpy(out, e, sizeof *out);
                return 0;
            }
        }
    }
    return -1;
}

/* ---- vfs_ops_t ---- */

static int fat_readdir(vfs_node_t *dir, uint32_t index, char *name_out, uint32_t *type_out) {
    fat_state_t *st = (fat_state_t *)dir->mount->fs_data;
    uint8_t buf[512];
    uint32_t seen = 0;

    for (uint32_t i = 0; i < st->root_dir_sectors; i++) {
        if (st->dev->read_sector(st->dev, st->root_dir_lba + i, buf)) return -1;
        for (int j = 0; j < 512; j += 32) {
            fat_dirent_t *e = (fat_dirent_t *)(buf + j);
            if ((uint8_t)e->name[0] == 0x00) return -1;
            if ((uint8_t)e->name[0] == 0xE5) continue;
            if (e->attr & FAT_ATTR_VOLUME_ID) continue;
            if (e->attr == 0x0F) continue;
            if (seen == index) {
                fat_name_to_dot(e->name, name_out, VFS_MAX_NAME);
                *type_out = (e->attr & FAT_ATTR_DIR) ? VFS_DIR : VFS_FILE;
                return 0;
            }
            seen++;
        }
    }
    return -1;
}

static vfs_node_t *fat_finddir(vfs_node_t *dir, const char *name) {
    fat_state_t *st = (fat_state_t *)dir->mount->fs_data;
    fat_dirent_t e;
    if (fat_lookup(st, name, &e) != 0) return NULL;

    vfs_node_t *node = kmalloc(sizeof(vfs_node_t));
    if (!node) return NULL;
    memset(node, 0, sizeof *node);
    fat_name_to_dot(e.name, node->name, VFS_MAX_NAME);
    node->type = (e.attr & FAT_ATTR_DIR) ? VFS_DIR : VFS_FILE;
    node->size = e.size;
    node->inode = ((uint32_t)e.first_cluster_hi << 16) | e.first_cluster_lo;
    node->ops = &fat_ops;
    node->mount = dir->mount;
    node->private_data = NULL;
    return node;
}

static ssize_t fat_read(vfs_node_t *node, void *buf, size_t size, off_t offset) {
    fat_state_t *st = (fat_state_t *)node->mount->fs_data;
    if (node->type != VFS_FILE) return -1;
    if (offset >= node->size) return 0;
    if (offset + size > node->size) size = node->size - offset;

    uint16_t cluster = (uint16_t)node->inode;

    /* Hard cap on cluster walks: a linear file visits each cluster at
       most once, so more than that means a corrupt circular chain.
       `+2` is the cluster-number offset (clusters start at 2). */
    uint32_t max_steps = st->total_clusters + 2;

    uint32_t bpc = st->bytes_per_cluster;
    uint32_t skip = offset / bpc;
    for (uint32_t i = 0; i < skip && cluster; i++) {
        if (i > max_steps) return 0;   /* bail on likely loop */
        cluster = fat_next_cluster(st, cluster);
    }
    if (!cluster) return 0;

    uint32_t cluster_off = offset % bpc;
    size_t done = 0;
    uint8_t sect_buf[512];
    uint32_t steps = 0;

    while (done < size && cluster) {
        if (steps++ > max_steps) break;
        /* cluster has already been bounds-checked by fat_next_cluster
           on return, but the initial cluster from node->inode hasn't;
           recheck to be safe. */
        if (cluster < 2 || cluster > st->total_clusters + 1) break;
        uint32_t lba = st->data_start_lba + ((uint32_t)(cluster - 2) * st->sectors_per_cluster);
        if (lba + st->sectors_per_cluster > st->part_end_lba) break;

        for (uint32_t s = 0; s < st->sectors_per_cluster && done < size; s++) {
            uint32_t sect_start = s * 512;
            uint32_t sect_end = sect_start + 512;
            if (sect_end <= cluster_off) continue;
            if (st->dev->read_sector(st->dev, lba + s, sect_buf)) return (ssize_t)done;
            uint32_t copy_from = 0;
            if (cluster_off > sect_start) copy_from = cluster_off - sect_start;
            uint32_t avail = 512 - copy_from;
            uint32_t want = size - done;
            uint32_t chunk = avail < want ? avail : want;
            memcpy((uint8_t *)buf + done, sect_buf + copy_from, chunk);
            done += chunk;
        }
        cluster_off = 0;
        cluster = fat_next_cluster(st, cluster);
    }
    return (ssize_t)done;
}

static int fat_stat(vfs_node_t *node, struct vfs_stat *out) {
    out->inode = node->inode;
    out->type = node->type;
    out->size = node->size;
    return 0;
}

static vfs_ops_t fat_ops = {
    .read    = fat_read,
    .write   = NULL,
    .open    = NULL,
    .close   = NULL,
    .readdir = fat_readdir,
    .finddir = fat_finddir,
    .create  = NULL,
    .mkdir   = NULL,
    .unlink  = NULL,
    .stat    = fat_stat,
};

vfs_ops_t *fat_get_ops(void) { return &fat_ops; }

vfs_node_t *fat_mount(blkdev_t *dev) {
    uint8_t bs[512];
    if (dev->read_sector(dev, 0, bs)) return NULL;
    fat_bpb_t *bpb = (fat_bpb_t *)bs;

    /* Minimal BPB sanity. Bail on anything dubious rather than guessing
       at a broken layout — bad values lead to garbage reads later. */
    if (bpb->bytes_per_sector != 512) return NULL;
    if (bpb->sectors_per_cluster == 0) return NULL;
    if (bpb->reserved_sectors == 0) return NULL;
    if (bpb->num_fats == 0) return NULL;
    if (bpb->sectors_per_fat_16 == 0) return NULL;
    if (bpb->root_entries == 0) return NULL;

    uint32_t fat_size    = bpb->sectors_per_fat_16;
    uint32_t fat_start   = bpb->reserved_sectors;
    uint32_t root_lba    = fat_start + (uint32_t)bpb->num_fats * fat_size;
    uint32_t root_sects  = ((uint32_t)bpb->root_entries * 32 + 511) / 512;
    uint32_t data_start  = root_lba + root_sects;

    /* Entire laid-out area must fit inside the partition's sector
       count. dev->total_sectors is the partition size (our
       blkdev_partition wrapper shows that, not the underlying disk). */
    if (data_start >= dev->total_sectors) return NULL;

    uint32_t data_sects  = dev->total_sectors - data_start;
    uint32_t total_clusters = data_sects / bpb->sectors_per_cluster;

    fat_state_t *st = kmalloc(sizeof *st);
    if (!st) return NULL;
    memset(st, 0, sizeof *st);
    st->dev = dev;
    st->sectors_per_cluster = bpb->sectors_per_cluster;
    st->bytes_per_cluster   = (uint32_t)bpb->sectors_per_cluster * 512;
    st->fat_start_lba       = fat_start;
    st->root_dir_lba        = root_lba;
    st->root_entries        = bpb->root_entries;
    st->root_dir_sectors    = root_sects;
    st->data_start_lba      = data_start;
    st->total_clusters      = total_clusters;
    st->part_end_lba        = dev->total_sectors;

    serial_printf("[fat] mounted: cluster=%u root@%u data@%u clusters=%u\n",
                  st->bytes_per_cluster, st->root_dir_lba,
                  st->data_start_lba, st->total_clusters);

    vfs_node_t *root = kmalloc(sizeof(vfs_node_t));
    if (!root) { kfree(st); return NULL; }
    memset(root, 0, sizeof *root);
    strncpy(root->name, "/", VFS_MAX_NAME);
    root->type = VFS_DIR;
    root->inode = 0;          /* root has no cluster; use 0 as sentinel */
    root->ops = &fat_ops;
    root->private_data = st;
    return root;
}
