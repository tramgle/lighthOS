#ifndef MULTIBOOT_H
#define MULTIBOOT_H

#include "include/types.h"

#define MULTIBOOT_MAGIC 0x2BADB002

typedef struct {
    uint32_t flags;
    uint32_t mem_lower;
    uint32_t mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;
    uint32_t mmap_addr;
    uint32_t drives_length;
    uint32_t drives_addr;
    uint32_t config_table;
    uint32_t boot_loader_name;
    uint32_t apm_table;
} __attribute__((packed)) multiboot_info_t;

typedef struct {
    uint32_t size;
    uint64_t addr;
    uint64_t len;
    uint32_t type;
} __attribute__((packed)) multiboot_mmap_entry_t;

typedef struct {
    uint32_t mod_start;   /* physical address of module start */
    uint32_t mod_end;     /* physical address of module end */
    uint32_t cmdline;     /* module command line string (physical addr) */
    uint32_t reserved;
} __attribute__((packed)) multiboot_mod_t;

#define MULTIBOOT_MMAP_AVAILABLE 1
#define MULTIBOOT_FLAG_CMDLINE (1 << 2)
#define MULTIBOOT_FLAG_MODS    (1 << 3)

#endif
