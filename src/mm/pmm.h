#ifndef PMM_H
#define PMM_H

#include "include/types.h"
#include "include/multiboot.h"

#define PAGE_SIZE 4096

/* Physical addresses are 64-bit; frame numbers are 32-bit (fits a
   2^32 × 4 KiB = 16 TiB address space — plenty for a hobby kernel). */

void     pmm_init(multiboot_info_t *mbi);
uint64_t pmm_alloc_frame(void);
void     pmm_free_frame(uint64_t addr);
uint32_t pmm_get_free_count(void);
uint32_t pmm_get_total_count(void);
void     pmm_reserve_range(uint64_t start, uint64_t size);

void     pmm_region_iter(void (*cb)(uint32_t start_frame, uint32_t len, bool used));

#endif
