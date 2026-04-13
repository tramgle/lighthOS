#ifndef VMM_H
#define VMM_H

#include "include/types.h"

#define VMM_FLAG_PRESENT  0x01
#define VMM_FLAG_WRITE    0x02
#define VMM_FLAG_USER     0x04

void     vmm_init(void);
void     vmm_map_page(uint32_t virt, uint32_t phys, uint32_t flags);
void     vmm_unmap_page(uint32_t virt);
uint32_t vmm_get_physical(uint32_t virt);

#endif
