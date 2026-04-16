/* FAT16/32 driver for LighthOS.
 *
 * Read + write support with 8.3 short names. Auto-detects FAT16 vs
 * FAT32 from the BPB at mount time. FAT32 uses 32-bit cluster entries
 * (top 4 bits reserved) and a cluster-chain root directory; FAT16
 * uses 16-bit entries and a fixed-size root. Both variants support
 * subdirectories, create, mkdir, write, unlink. Both FAT copies are
 * kept in sync. No LFN, no FAT12/exFAT. */

#include "fs/fat.h"
#include "mm/heap.h"
#include "lib/string.h"
#include "lib/kprintf.h"

#define FAT_ATTR_DIR       0x10
#define FAT_ATTR_VOLUME_ID 0x08
#define FAT16_EOC_MIN      0xFFF8
#define FAT32_EOC_MIN      0x0FFFFFF8u
#define FAT32_EOC          0x0FFFFFFFu
#define FAT32_MASK         0x0FFFFFFFu

/* Common BPB (first 36 bytes), followed by the FAT16 or FAT32
   extended region. We read the common part first, then probe which
   variant to use based on cluster count. */
typedef struct {
    uint8_t  jmp[3];
    char     oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entries;
    uint16_t total_sectors_16;
    uint8_t  media_type;
    uint16_t sectors_per_fat_16;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
} __attribute__((packed)) fat_bpb_common_t;

typedef struct {
    fat_bpb_common_t common;
    /* FAT32-specific fields (byte offset 36..51 of the boot sector) */
    uint32_t sectors_per_fat_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info;
    uint16_t backup_boot_sector;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_sig;
    uint32_t volume_id;
    char     volume_label[11];
    char     fs_type[8];
} __attribute__((packed)) fat32_bpb_t;

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
    int       is_fat32;
    uint32_t  fat_start_lba;
    uint32_t  sectors_per_fat;
    uint32_t  num_fats;
    uint32_t  root_dir_lba;          /* FAT16 only: fixed root area */
    uint32_t  root_dir_sectors;      /* FAT16 only */
    uint32_t  root_entries;          /* FAT16 only */
    uint32_t  root_cluster;          /* FAT32 only: first cluster of root */
    uint32_t  data_start_lba;
    uint32_t  sectors_per_cluster;
    uint32_t  bytes_per_cluster;
    uint32_t  total_clusters;
    uint32_t  part_end_lba;
} fat_state_t;

static vfs_ops_t fat_ops;

static char to_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

/* Render an 11-byte FAT 8.3 name into `out` as lowercase
   (e.g. "HELLO   TXT" -> "hello.txt"). Standard UX convention. */
static void fat_name_to_dot(const char name[11], char *out, int out_size) {
    int i = 0, o = 0;
    while (i < 8 && name[i] != ' ' && o < out_size - 1)
        out[o++] = to_lower(name[i++]);
    if (name[8] != ' ') {
        if (o < out_size - 1) out[o++] = '.';
        int j = 0;
        while (j < 3 && name[8 + j] != ' ' && o < out_size - 1)
            out[o++] = to_lower(name[8 + j++]);
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

/* ---- Low-level FAT table helpers ---- */

/* Read a FAT entry. FAT16: 2-byte entries. FAT32: 4-byte entries
   (top 4 bits reserved). Returns 0 on EOC / out-of-range / error. */
static uint32_t fat_next_cluster(fat_state_t *st, uint32_t cluster) {
    if (cluster < 2 || cluster > st->total_clusters + 1) return 0;
    uint32_t entry_size = st->is_fat32 ? 4 : 2;
    uint32_t offset = cluster * entry_size;
    uint32_t sect = st->fat_start_lba + (offset / 512);
    uint32_t byte_off = offset % 512;
    uint8_t buf[512];
    if (st->dev->read_sector(st->dev, sect, buf)) return 0;
    uint32_t next;
    if (st->is_fat32) {
        next = buf[byte_off] | ((uint32_t)buf[byte_off+1] << 8) |
               ((uint32_t)buf[byte_off+2] << 16) | ((uint32_t)buf[byte_off+3] << 24);
        next &= FAT32_MASK;
        if (next >= FAT32_EOC_MIN) return 0;
    } else {
        next = buf[byte_off] | ((uint32_t)buf[byte_off+1] << 8);
        if (next >= FAT16_EOC_MIN) return 0;
    }
    if (next < 2 || next > st->total_clusters + 1) return 0;
    return next;
}

static int fat_set_cluster(fat_state_t *st, uint32_t cluster, uint32_t value) {
    uint32_t entry_size = st->is_fat32 ? 4 : 2;
    uint32_t offset = cluster * entry_size;
    uint32_t sect_off = offset / 512;
    uint32_t byte_off = offset % 512;
    uint8_t buf[512];
    for (uint32_t f = 0; f < st->num_fats; f++) {
        uint32_t sect = st->fat_start_lba + f * st->sectors_per_fat + sect_off;
        if (st->dev->read_sector(st->dev, sect, buf)) return -1;
        if (st->is_fat32) {
            uint32_t old = buf[byte_off] | ((uint32_t)buf[byte_off+1]<<8) |
                           ((uint32_t)buf[byte_off+2]<<16) | ((uint32_t)buf[byte_off+3]<<24);
            value = (old & ~FAT32_MASK) | (value & FAT32_MASK);
            buf[byte_off]   = (uint8_t)(value);
            buf[byte_off+1] = (uint8_t)(value >> 8);
            buf[byte_off+2] = (uint8_t)(value >> 16);
            buf[byte_off+3] = (uint8_t)(value >> 24);
        } else {
            buf[byte_off]   = (uint8_t)(value & 0xFF);
            buf[byte_off+1] = (uint8_t)(value >> 8);
        }
        if (st->dev->write_sector(st->dev, sect, buf)) return -1;
    }
    return 0;
}

static uint32_t fat_alloc_cluster(fat_state_t *st) {
    uint32_t entry_size = st->is_fat32 ? 4 : 2;
    uint8_t buf[512];
    uint32_t eoc = st->is_fat32 ? FAT32_EOC : 0xFFFF;
    for (uint32_t c = 2; c <= st->total_clusters + 1; c++) {
        uint32_t offset = c * entry_size;
        uint32_t sect = st->fat_start_lba + (offset / 512);
        uint32_t byte_off = offset % 512;
        if (st->dev->read_sector(st->dev, sect, buf)) return 0;
        uint32_t val;
        if (st->is_fat32) {
            val = buf[byte_off] | ((uint32_t)buf[byte_off+1]<<8) |
                  ((uint32_t)buf[byte_off+2]<<16) | ((uint32_t)buf[byte_off+3]<<24);
            val &= FAT32_MASK;
        } else {
            val = buf[byte_off] | ((uint32_t)buf[byte_off+1] << 8);
        }
        if (val == 0) {
            if (fat_set_cluster(st, c, eoc) != 0) return 0;
            uint32_t lba = st->data_start_lba + (c - 2) * st->sectors_per_cluster;
            uint8_t zero[512];
            memset(zero, 0, 512);
            for (uint32_t s = 0; s < st->sectors_per_cluster; s++)
                st->dev->write_sector(st->dev, lba + s, zero);
            return c;
        }
    }
    return 0;
}

static void fat_free_chain(fat_state_t *st, uint32_t start) {
    uint32_t c = start;
    while (c >= 2 && c <= st->total_clusters + 1) {
        uint32_t next = fat_next_cluster(st, c);
        fat_set_cluster(st, c, 0);
        if (!next) break;
        c = next;
    }
}

static uint32_t fat_cluster_to_lba(fat_state_t *st, uint32_t cluster) {
    return st->data_start_lba + ((uint32_t)(cluster - 2) * st->sectors_per_cluster);
}

/* Walk to the Nth cluster in a chain, allocating if `alloc` is true.
   Returns the cluster number, or 0 on failure. */
static uint32_t fat_walk_chain(fat_state_t *st, uint32_t start, uint32_t n, int alloc) {
    uint32_t c = start;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t next = fat_next_cluster(st, c);
        if (!next) {
            if (!alloc) return 0;
            next = fat_alloc_cluster(st);
            if (!next) return 0;
            fat_set_cluster(st, c, next);
        }
        c = next;
    }
    return c;
}

/* ---- Generic directory operations (root + subdirs) ---- */

/* A dir_ctx abstracts the iteration of either the fixed root directory
   or a cluster-chain subdirectory. For root: iterate root_dir_lba
   sectors. For subdir: iterate clusters from the node's inode field. */

typedef struct {
    fat_state_t *st;
    int is_root;
    uint32_t first_cluster;   /* subdir only */
} dir_ctx_t;

static void dir_ctx_init(dir_ctx_t *ctx, fat_state_t *st, vfs_node_t *dir) {
    ctx->st = st;
    /* FAT16 root: inode == 0 signals "use the fixed root area".
       FAT32 root: inode == root_cluster, which is a cluster chain
       like any subdirectory — no special fixed area. */
    ctx->is_root = (!st->is_fat32 && dir->inode == 0);
    ctx->first_cluster = dir->inode;
}

/* Callback for walking directory entries. Return 0 to continue, >0 to
   stop-and-succeed, <0 to stop-and-fail. `sector_lba` + `entry_off`
   identify the on-disk location for update. */
typedef int (*dir_walk_fn)(fat_dirent_t *e, uint32_t sector_lba,
                           int entry_off, void *arg);

/* VFAT LFN accumulator — collects long-name entries (attr 0x0F)
   preceding a real 8.3 dirent. Callbacks read lfn_active/lfn_buf
   for the full name when present. Single-threaded, file-local. */
#define LFN_MAX 256
static char lfn_buf[LFN_MAX];
static int  lfn_len;
static int  lfn_active;

static void lfn_reset(void) { lfn_len = 0; lfn_active = 0; lfn_buf[0] = '\0'; }

static void lfn_collect(const uint8_t *raw) {
    int seq = raw[0] & 0x3F;
    int start = (seq - 1) * 13;
    static const int off[] = {1,3,5,7,9, 14,16,18,20,22,24, 28,30};
    for (int i = 0; i < 13 && start + i < LFN_MAX - 1; i++) {
        uint16_t ch = raw[off[i]] | ((uint16_t)raw[off[i]+1] << 8);
        if (ch == 0 || ch == 0xFFFF) break;
        lfn_buf[start + i] = (char)(ch & 0x7F);
        if (start + i + 1 > lfn_len) lfn_len = start + i + 1;
    }
    lfn_buf[lfn_len] = '\0';
    lfn_active = 1;
}

static int dir_walk(dir_ctx_t *ctx, dir_walk_fn fn, void *arg) {
    fat_state_t *st = ctx->st;
    uint8_t buf[512];
    lfn_reset();

    /* Process one 32-byte slot: LFN entries accumulate; real entries
       are passed to the callback with lfn_buf set if available. */
    #define SLOT(ptr, lba, off) do {                               \
        fat_dirent_t *_e = (fat_dirent_t *)((ptr) + (off));       \
        if (_e->attr == 0x0F) {                                   \
            lfn_collect((const uint8_t *)_e);                     \
        } else {                                                  \
            int _rc = fn(_e, (lba), (off), arg);                  \
            lfn_reset();                                          \
            if (_rc != 0) return _rc;                             \
        }                                                         \
    } while (0)

    if (ctx->is_root) {
        for (uint32_t i = 0; i < st->root_dir_sectors; i++) {
            uint32_t lba = st->root_dir_lba + i;
            if (st->dev->read_sector(st->dev, lba, buf)) return -1;
            for (int j = 0; j < 512; j += 32) SLOT(buf, lba, j);
        }
    } else {
        uint32_t c = ctx->first_cluster;
        uint32_t max = st->total_clusters + 2, steps = 0;
        while (c >= 2 && c <= st->total_clusters + 1 && steps++ < max) {
            uint32_t base = fat_cluster_to_lba(st, c);
            for (uint32_t s = 0; s < st->sectors_per_cluster; s++) {
                uint32_t lba = base + s;
                if (st->dev->read_sector(st->dev, lba, buf)) return -1;
                for (int j = 0; j < 512; j += 32) SLOT(buf, lba, j);
            }
            c = fat_next_cluster(st, c);
        }
    }
    #undef SLOT
    return 0;
}

/* ---- Callbacks used by the VFS operations ---- */

typedef struct { uint32_t target; uint32_t seen; char name[VFS_MAX_NAME]; uint32_t type; } readdir_arg_t;

static int cb_readdir(fat_dirent_t *e, uint32_t lba, int off, void *a) {
    (void)lba; (void)off;
    readdir_arg_t *arg = (readdir_arg_t *)a;
    if ((uint8_t)e->name[0] == 0x00) return -1;
    if ((uint8_t)e->name[0] == 0xE5) return 0;
    if (e->attr & FAT_ATTR_VOLUME_ID) return 0;
    if (arg->seen == arg->target) {
        if (lfn_active && lfn_len > 0)
            strncpy(arg->name, lfn_buf, VFS_MAX_NAME - 1);
        else
            fat_name_to_dot(e->name, arg->name, VFS_MAX_NAME);
        arg->name[VFS_MAX_NAME - 1] = '\0';
        arg->type = (e->attr & FAT_ATTR_DIR) ? VFS_DIR : VFS_FILE;
        return 1;
    }
    arg->seen++;
    return 0;
}

typedef struct { char target[11]; char orig_name[VFS_MAX_NAME]; fat_dirent_t out; int found; } lookup_arg_t;

static int cb_lookup(fat_dirent_t *e, uint32_t lba, int off, void *a) {
    (void)lba; (void)off;
    lookup_arg_t *arg = (lookup_arg_t *)a;
    if ((uint8_t)e->name[0] == 0x00) return -1;
    if ((uint8_t)e->name[0] == 0xE5) return 0;
    if (e->attr & FAT_ATTR_VOLUME_ID) return 0;
    /* Match against 8.3 name OR the LFN (case-insensitive). */
    int match = (memcmp(e->name, arg->target, 11) == 0);
    if (!match && lfn_active && lfn_len > 0) {
        /* Case-insensitive compare of LFN against the ORIGINAL
           (pre-8.3) user name. */
        const char *orig = arg->orig_name;
        int i = 0;
        match = 1;
        while (lfn_buf[i] || orig[i]) {
            char a_c = lfn_buf[i], b_c = orig[i];
            if (a_c >= 'A' && a_c <= 'Z') a_c += 32;
            if (b_c >= 'A' && b_c <= 'Z') b_c += 32;
            if (a_c != b_c) { match = 0; break; }
            i++;
        }
    }
    if (match) {
        memcpy(&arg->out, e, sizeof(fat_dirent_t));
        arg->found = 1;
        return 1;
    }
    return 0;
}

/* Find a free directory entry slot (0x00 or 0xE5). Writes `ent` into
   that slot. For subdirs, allocates a new cluster if needed. */
typedef struct {
    fat_dirent_t *ent;
    int written;
    fat_state_t *st;
} create_arg_t;

static int cb_find_free_slot(fat_dirent_t *e, uint32_t lba, int off, void *a) {
    create_arg_t *arg = (create_arg_t *)a;
    if ((uint8_t)e->name[0] == 0x00 || (uint8_t)e->name[0] == 0xE5) {
        /* Write the new entry at this position. Read the sector, patch
           the 32-byte slot, write back. */
        uint8_t buf[512];
        if (arg->st->dev->read_sector(arg->st->dev, lba, buf)) return -1;
        memcpy(buf + off, arg->ent, 32);
        if (arg->st->dev->write_sector(arg->st->dev, lba, buf)) return -1;
        arg->written = 1;
        return 1;
    }
    return 0;
}

static int fat_lookup_in(fat_state_t *st, vfs_node_t *dir, const char *name,
                         fat_dirent_t *out) {
    dir_ctx_t ctx; dir_ctx_init(&ctx, st, dir);
    lookup_arg_t arg;
    fat_name_from_dot(name, arg.target);
    strncpy(arg.orig_name, name, VFS_MAX_NAME - 1);
    arg.orig_name[VFS_MAX_NAME - 1] = '\0';
    arg.found = 0;
    dir_walk(&ctx, cb_lookup, &arg);
    if (arg.found) { memcpy(out, &arg.out, sizeof *out); return 0; }
    return -1;
}

/* ---- vfs_ops_t implementations ---- */

static int fat_readdir(vfs_node_t *dir, uint32_t index, char *name_out, uint32_t *type_out) {
    fat_state_t *st = (fat_state_t *)dir->mount->fs_data;
    dir_ctx_t ctx; dir_ctx_init(&ctx, st, dir);
    readdir_arg_t arg = { .target = index, .seen = 0 };
    int rc = dir_walk(&ctx, cb_readdir, &arg);
    if (rc > 0) {
        strncpy(name_out, arg.name, VFS_MAX_NAME - 1);
        name_out[VFS_MAX_NAME - 1] = '\0';
        *type_out = arg.type;
        return 0;
    }
    return -1;
}

/* Per-node context: remembers the parent dir's first cluster so we
   can flush size updates back to the directory entry on disk. */
typedef struct {
    uint32_t parent_first_cluster; /* 0 = parent is root dir (FAT16) or root_cluster (FAT32) */
} fat_node_ctx_t;

static vfs_node_t *fat_finddir(vfs_node_t *dir, const char *name) {
    fat_state_t *st = (fat_state_t *)dir->mount->fs_data;
    fat_dirent_t e;
    if (fat_lookup_in(st, dir, name, &e) != 0) return NULL;

    vfs_node_t *node = kmalloc(sizeof(vfs_node_t));
    if (!node) return NULL;
    memset(node, 0, sizeof *node);
    /* Use the original lookup name — preserves case and long names. */
    strncpy(node->name, name, VFS_MAX_NAME - 1);
    node->name[VFS_MAX_NAME - 1] = '\0';
    node->type = (e.attr & FAT_ATTR_DIR) ? VFS_DIR : VFS_FILE;
    node->size = e.size;
    node->inode = ((uint32_t)e.first_cluster_hi << 16) | e.first_cluster_lo;
    node->ops = &fat_ops;
    node->mount = dir->mount;

    fat_node_ctx_t *ctx = kmalloc(sizeof(fat_node_ctx_t));
    if (ctx) {
        ctx->parent_first_cluster = dir->inode;
        node->private_data = ctx;
    }
    return node;
}

static ssize_t fat_read(vfs_node_t *node, void *buf, size_t size, off_t offset) {
    fat_state_t *st = (fat_state_t *)node->mount->fs_data;
    if (node->type != VFS_FILE) return -1;
    if ((uint32_t)offset >= node->size) return 0;
    if (offset + size > node->size) size = node->size - offset;

    uint32_t cluster = node->inode;
    if (!cluster) return 0;
    uint32_t bpc = st->bytes_per_cluster;
    uint32_t skip = (uint32_t)offset / bpc;
    cluster = skip ? fat_walk_chain(st, cluster, skip, 0) : cluster;
    if (!cluster) return 0;

    uint32_t cluster_off = (uint32_t)offset % bpc;
    size_t done = 0;
    uint8_t sect_buf[512];
    uint32_t max_steps = st->total_clusters + 2, steps = 0;

    while (done < size && cluster && steps++ < max_steps) {
        if (cluster < 2 || cluster > st->total_clusters + 1) break;
        uint32_t lba = fat_cluster_to_lba(st, cluster);
        for (uint32_t s = 0; s < st->sectors_per_cluster && done < size; s++) {
            uint32_t sect_start = s * 512;
            if (sect_start + 512 <= cluster_off) continue;
            if (st->dev->read_sector(st->dev, lba + s, sect_buf)) return (ssize_t)done;
            uint32_t copy_from = (cluster_off > sect_start) ? cluster_off - sect_start : 0;
            uint32_t chunk = 512 - copy_from;
            if (chunk > size - done) chunk = size - done;
            memcpy((uint8_t *)buf + done, sect_buf + copy_from, chunk);
            done += chunk;
        }
        cluster_off = 0;
        cluster = fat_next_cluster(st, cluster);
    }
    return (ssize_t)done;
}

static void fat_flush_dirent(fat_state_t *st, vfs_node_t *parent,
                             const char *name, uint32_t size, uint32_t first_cluster);

static ssize_t fat_write(vfs_node_t *node, const void *buf, size_t size, off_t offset) {
    fat_state_t *st = (fat_state_t *)node->mount->fs_data;
    if (node->type != VFS_FILE) return -1;

    uint32_t bpc = st->bytes_per_cluster;
    uint32_t cluster = node->inode;

    /* Allocate first cluster if the file was empty. */
    if (!cluster) {
        cluster = fat_alloc_cluster(st);
        if (!cluster) return -1;
        node->inode = cluster;
    }

    uint32_t skip = (uint32_t)offset / bpc;
    cluster = skip ? fat_walk_chain(st, cluster, skip, 1) : cluster;
    if (!cluster) return -1;

    uint32_t cluster_off = (uint32_t)offset % bpc;
    size_t done = 0;
    uint8_t sect_buf[512];
    uint32_t max_steps = st->total_clusters + 2, steps = 0;

    while (done < size && cluster && steps++ < max_steps) {
        uint32_t lba = fat_cluster_to_lba(st, cluster);
        for (uint32_t s = 0; s < st->sectors_per_cluster && done < size; s++) {
            uint32_t sect_start = s * 512;
            if (sect_start + 512 <= cluster_off) continue;
            uint32_t copy_to = (cluster_off > sect_start) ? cluster_off - sect_start : 0;
            uint32_t chunk = 512 - copy_to;
            if (chunk > size - done) chunk = size - done;
            /* Read-modify-write for partial sectors. */
            if (st->dev->read_sector(st->dev, lba + s, sect_buf)) return (ssize_t)done;
            memcpy(sect_buf + copy_to, (const uint8_t *)buf + done, chunk);
            if (st->dev->write_sector(st->dev, lba + s, sect_buf)) return (ssize_t)done;
            done += chunk;
        }
        cluster_off = 0;
        /* Need more clusters? Allocate on the fly. */
        if (done < size) {
            uint32_t next = fat_next_cluster(st, cluster);
            if (!next) {
                next = fat_alloc_cluster(st);
                if (!next) break;
                fat_set_cluster(st, cluster, next);
            }
            cluster = next;
        }
    }

    /* Update file size if we extended. */
    uint32_t end = (uint32_t)offset + done;
    if (end > node->size) node->size = end;

    /* Flush updated size + first_cluster back to the on-disk dir
       entry. Build a temporary parent vfs_node_t so we can use the
       generic dir-walk to find and patch the entry. */
    fat_node_ctx_t *nctx = (fat_node_ctx_t *)node->private_data;
    uint32_t parent_cluster = nctx ? nctx->parent_first_cluster : 0;
    vfs_node_t fake_parent;
    memset(&fake_parent, 0, sizeof fake_parent);
    fake_parent.inode = parent_cluster;
    fake_parent.mount = node->mount;
    fat_flush_dirent(st, &fake_parent, node->name,
                     node->size, node->inode);

    return (ssize_t)done;
}

/* Update a file's dir entry on disk (size + first_cluster). Called by
   the VFS close path or explicitly after writes. Scans the parent
   directory to find the entry. */
/* Flush callback: find matching name and patch size + first_cluster. */
typedef struct { char target[11]; uint32_t size; uint32_t first_cluster; fat_state_t *st; int done; } flush_arg_t;

static int cb_flush(fat_dirent_t *e, uint32_t lba, int off, void *a) {
    flush_arg_t *arg = (flush_arg_t *)a;
    if ((uint8_t)e->name[0] == 0x00) return -1;
    if ((uint8_t)e->name[0] == 0xE5) return 0;
    if (memcmp(e->name, arg->target, 11) != 0) return 0;

    uint8_t buf[512];
    if (arg->st->dev->read_sector(arg->st->dev, lba, buf)) return -1;
    fat_dirent_t *de = (fat_dirent_t *)(buf + off);
    de->size = arg->size;
    de->first_cluster_lo = (uint16_t)(arg->first_cluster & 0xFFFF);
    de->first_cluster_hi = (uint16_t)(arg->first_cluster >> 16);
    if (arg->st->dev->write_sector(arg->st->dev, lba, buf)) return -1;
    arg->done = 1;
    return 1;
}

static void fat_flush_dirent(fat_state_t *st, vfs_node_t *parent,
                             const char *name, uint32_t size, uint32_t first_cluster) {
    dir_ctx_t ctx; dir_ctx_init(&ctx, st, parent);
    flush_arg_t arg;
    fat_name_from_dot(name, arg.target);
    arg.size = size;
    arg.first_cluster = first_cluster;
    arg.st = st;
    arg.done = 0;
    dir_walk(&ctx, cb_flush, &arg);
}

static int fat_create(vfs_node_t *dir, const char *name, uint32_t type) {
    fat_state_t *st = (fat_state_t *)dir->mount->fs_data;

    /* Check for duplicate. */
    fat_dirent_t existing;
    if (fat_lookup_in(st, dir, name, &existing) == 0) return -1;

    fat_dirent_t ent;
    memset(&ent, 0, sizeof ent);
    fat_name_from_dot(name, ent.name);
    ent.attr = (type == VFS_DIR) ? FAT_ATTR_DIR : 0;
    ent.size = 0;
    ent.first_cluster_lo = 0;
    ent.first_cluster_hi = 0;

    if (type == VFS_DIR) {
        uint32_t c = fat_alloc_cluster(st);
        if (!c) return -1;
        ent.first_cluster_lo = (uint16_t)(c & 0xFFFF);
        ent.first_cluster_hi = (uint16_t)(c >> 16);

        /* Write "." and ".." entries in the new dir's cluster. */
        uint8_t dbuf[512];
        memset(dbuf, 0, 512);
        fat_dirent_t *dot = (fat_dirent_t *)dbuf;
        memcpy(dot->name, ".          ", 11);
        dot->attr = FAT_ATTR_DIR;
        dot->first_cluster_lo = (uint16_t)(c & 0xFFFF);
        dot->first_cluster_hi = (uint16_t)(c >> 16);

        fat_dirent_t *dotdot = (fat_dirent_t *)(dbuf + 32);
        memcpy(dotdot->name, "..         ", 11);
        dotdot->attr = FAT_ATTR_DIR;
        dotdot->first_cluster_lo = (uint16_t)(dir->inode & 0xFFFF);
        dotdot->first_cluster_hi = (uint16_t)(dir->inode >> 16);

        uint32_t lba = fat_cluster_to_lba(st, c);
        st->dev->write_sector(st->dev, lba, dbuf);
    }

    dir_ctx_t ctx; dir_ctx_init(&ctx, st, dir);
    create_arg_t arg = { .ent = &ent, .written = 0, .st = st };
    dir_walk(&ctx, cb_find_free_slot, &arg);

    if (!arg.written && !ctx.is_root) {
        /* Subdir: allocate a new cluster for more entries. */
        uint32_t last = ctx.first_cluster;
        while (fat_next_cluster(st, last)) last = fat_next_cluster(st, last);
        uint32_t nc = fat_alloc_cluster(st);
        if (!nc) return -1;
        fat_set_cluster(st, last, nc);
        /* Write entry at start of new cluster. */
        uint8_t fresh[512];
        memset(fresh, 0, 512);
        memcpy(fresh, &ent, 32);
        st->dev->write_sector(st->dev, fat_cluster_to_lba(st, nc), fresh);
        arg.written = 1;
    }

    return arg.written ? 0 : -1;
}

static int fat_mkdir(vfs_node_t *dir, const char *name) {
    return fat_create(dir, name, VFS_DIR);
}

/* For unlink: find the entry, mark 0xE5, free the cluster chain. */
typedef struct { char target[11]; fat_state_t *st; int done; } unlink_arg_t;

static int cb_unlink(fat_dirent_t *e, uint32_t lba, int off, void *a) {
    unlink_arg_t *arg = (unlink_arg_t *)a;
    if ((uint8_t)e->name[0] == 0x00) return -1;
    if ((uint8_t)e->name[0] == 0xE5) return 0;
    if (memcmp(e->name, arg->target, 11) != 0) return 0;

    uint32_t first = (uint32_t)e->first_cluster_lo | ((uint32_t)e->first_cluster_hi << 16);
    if (first >= 2) fat_free_chain(arg->st, first);

    uint8_t buf[512];
    if (arg->st->dev->read_sector(arg->st->dev, lba, buf)) return -1;
    buf[off] = 0xE5;
    if (arg->st->dev->write_sector(arg->st->dev, lba, buf)) return -1;
    arg->done = 1;
    return 1;
}

static int fat_unlink(vfs_node_t *dir, const char *name) {
    fat_state_t *st = (fat_state_t *)dir->mount->fs_data;
    dir_ctx_t ctx; dir_ctx_init(&ctx, st, dir);
    unlink_arg_t arg;
    fat_name_from_dot(name, arg.target);
    arg.st = st;
    arg.done = 0;
    dir_walk(&ctx, cb_unlink, &arg);
    return arg.done ? 0 : -1;
}

static int fat_stat(vfs_node_t *node, struct vfs_stat *out) {
    out->inode = node->inode;
    out->type = node->type;
    out->size = node->size;
    return 0;
}

static vfs_ops_t fat_ops = {
    .read    = fat_read,
    .write   = fat_write,
    .open    = NULL,
    .close   = NULL,
    .readdir = fat_readdir,
    .finddir = fat_finddir,
    .create  = fat_create,
    .mkdir   = fat_mkdir,
    .unlink  = fat_unlink,
    .stat    = fat_stat,
};

vfs_ops_t *fat_get_ops(void) { return &fat_ops; }

vfs_node_t *fat_mount(blkdev_t *dev) {
    uint8_t bs[512];
    if (dev->read_sector(dev, 0, bs)) return NULL;
    fat_bpb_common_t *bpb = (fat_bpb_common_t *)bs;

    if (bpb->bytes_per_sector != 512) return NULL;
    if (bpb->sectors_per_cluster == 0) return NULL;
    if (bpb->reserved_sectors == 0) return NULL;
    if (bpb->num_fats == 0) return NULL;

    uint32_t fat_size = bpb->sectors_per_fat_16;
    int is_fat32 = 0;
    uint32_t root_cluster = 0;

    if (fat_size == 0) {
        /* FAT32: sectors_per_fat_16 is 0, use the 32-bit field. */
        fat32_bpb_t *bpb32 = (fat32_bpb_t *)bs;
        fat_size = bpb32->sectors_per_fat_32;
        root_cluster = bpb32->root_cluster;
        is_fat32 = 1;
        if (fat_size == 0) return NULL;
    } else {
        if (bpb->root_entries == 0) return NULL;
    }

    uint32_t fat_start  = bpb->reserved_sectors;
    uint32_t root_lba   = fat_start + (uint32_t)bpb->num_fats * fat_size;
    uint32_t root_sects = is_fat32 ? 0 :
                          ((uint32_t)bpb->root_entries * 32 + 511) / 512;
    uint32_t data_start = root_lba + root_sects;

    uint32_t total_sects = bpb->total_sectors_16 ? bpb->total_sectors_16
                                                  : bpb->total_sectors_32;
    if (data_start >= total_sects) return NULL;

    uint32_t data_sects    = total_sects - data_start;
    uint32_t total_clusters = data_sects / bpb->sectors_per_cluster;

    fat_state_t *st = kmalloc(sizeof *st);
    if (!st) return NULL;
    memset(st, 0, sizeof *st);
    st->dev                 = dev;
    st->is_fat32            = is_fat32;
    st->sectors_per_cluster = bpb->sectors_per_cluster;
    st->bytes_per_cluster   = (uint32_t)bpb->sectors_per_cluster * 512;
    st->fat_start_lba       = fat_start;
    st->sectors_per_fat     = fat_size;
    st->num_fats            = bpb->num_fats;
    st->root_dir_lba        = is_fat32 ? 0 : root_lba;
    st->root_entries        = is_fat32 ? 0 : bpb->root_entries;
    st->root_dir_sectors    = root_sects;
    st->root_cluster        = root_cluster;
    st->data_start_lba      = data_start;
    st->total_clusters      = total_clusters;
    st->part_end_lba        = dev->total_sectors;

    serial_printf("[fat] mounted %s: cluster=%u data@%u clusters=%u\n",
                  is_fat32 ? "FAT32" : "FAT16",
                  st->bytes_per_cluster,
                  st->data_start_lba, st->total_clusters);

    vfs_node_t *root = kmalloc(sizeof(vfs_node_t));
    if (!root) { kfree(st); return NULL; }
    memset(root, 0, sizeof *root);
    strncpy(root->name, "/", VFS_MAX_NAME);
    root->type = VFS_DIR;
    /* For FAT32 the root is a normal cluster chain; for FAT16 inode=0
       signals "use the fixed root area" (handled in dir_ctx_init). */
    root->inode = is_fat32 ? root_cluster : 0;
    root->ops = &fat_ops;
    root->private_data = st;
    return root;
}
