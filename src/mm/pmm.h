#ifndef PMM_H
#define PMM_H

#include "include/types.h"
#include "include/multiboot.h"

#define PAGE_SIZE 4096

void     pmm_init(multiboot_info_t *mbi);
uint32_t pmm_alloc_frame(void);
void     pmm_free_frame(uint32_t addr);
uint32_t pmm_get_free_count(void);
uint32_t pmm_get_total_count(void);
/* Mark all frames in [start, start+size) as in-use so pmm_alloc_frame
   never hands them back. Used to cordon off the kernel heap region. */
void     pmm_reserve_range(uint32_t start, uint32_t size);

/* Walk the bitmap and emit contiguous same-state runs.
   `cb` is invoked with (start_frame, length_in_frames, used). */
void     pmm_region_iter(void (*cb)(uint32_t start_frame, uint32_t len, bool used));

#endif
