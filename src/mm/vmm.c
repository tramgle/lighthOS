#include "mm/vmm.h"
#include "mm/pmm.h"
#include "lib/string.h"
#include "lib/kprintf.h"

/* Page directory: 1024 entries, each pointing to a page table */
static uint32_t page_directory[1024] __attribute__((aligned(4096)));

/* We need enough page tables to identity-map kernel + some headroom.
   Pre-allocate page tables for the first 16MB (4 page tables). */
static uint32_t page_tables[4][1024] __attribute__((aligned(4096)));

void vmm_init(void) {
    memset(page_directory, 0, sizeof(page_directory));

    /* Identity map first 16MB using pre-allocated page tables.
       This covers: BIOS/VGA (0-1MB), kernel (1MB+), and headroom. */
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 1024; j++) {
            uint32_t phys = (i * 1024 + j) * PAGE_SIZE;
            page_tables[i][j] = phys | VMM_FLAG_PRESENT | VMM_FLAG_WRITE;
        }
        page_directory[i] = (uint32_t)&page_tables[i] | VMM_FLAG_PRESENT | VMM_FLAG_WRITE;
    }

    /* Load page directory into CR3 */
    __asm__ volatile ("mov %0, %%cr3" :: "r"(page_directory));

    /* Enable paging */
    uint32_t cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000;
    __asm__ volatile ("mov %0, %%cr0" :: "r"(cr0));

    serial_printf("[vmm] Paging enabled, first 16MB identity-mapped\n");
}

void vmm_map_page(uint32_t virt, uint32_t phys, uint32_t flags) {
    uint32_t pd_idx = virt >> 22;
    uint32_t pt_idx = (virt >> 12) & 0x3FF;

    /* If no page table exists for this PD entry, allocate one */
    if (!(page_directory[pd_idx] & VMM_FLAG_PRESENT)) {
        uint32_t pt_phys = pmm_alloc_frame();
        if (!pt_phys) {
            kprintf("vmm: out of memory for page table\n");
            return;
        }
        memset((void *)pt_phys, 0, PAGE_SIZE);
        page_directory[pd_idx] = pt_phys | VMM_FLAG_PRESENT | VMM_FLAG_WRITE | (flags & VMM_FLAG_USER);
    }

    uint32_t *pt = (uint32_t *)(page_directory[pd_idx] & 0xFFFFF000);
    pt[pt_idx] = (phys & 0xFFFFF000) | (flags & 0xFFF) | VMM_FLAG_PRESENT;

    /* Invalidate TLB for this page */
    __asm__ volatile ("invlpg (%0)" :: "r"(virt) : "memory");
}

void vmm_unmap_page(uint32_t virt) {
    uint32_t pd_idx = virt >> 22;
    uint32_t pt_idx = (virt >> 12) & 0x3FF;

    if (!(page_directory[pd_idx] & VMM_FLAG_PRESENT)) return;

    uint32_t *pt = (uint32_t *)(page_directory[pd_idx] & 0xFFFFF000);
    pt[pt_idx] = 0;

    __asm__ volatile ("invlpg (%0)" :: "r"(virt) : "memory");
}

uint32_t vmm_get_physical(uint32_t virt) {
    uint32_t pd_idx = virt >> 22;
    uint32_t pt_idx = (virt >> 12) & 0x3FF;

    if (!(page_directory[pd_idx] & VMM_FLAG_PRESENT)) return 0;

    uint32_t *pt = (uint32_t *)(page_directory[pd_idx] & 0xFFFFF000);
    if (!(pt[pt_idx] & VMM_FLAG_PRESENT)) return 0;

    return (pt[pt_idx] & 0xFFFFF000) | (virt & 0xFFF);
}
