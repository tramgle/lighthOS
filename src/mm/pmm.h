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

#endif
