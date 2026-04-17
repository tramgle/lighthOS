#ifndef VMM_H
#define VMM_H

#include "include/types.h"

/* x86_64 paging flags (shared for all four levels of tables). */
#define VMM_FLAG_PRESENT  0x001ULL
#define VMM_FLAG_WRITE    0x002ULL
#define VMM_FLAG_USER     0x004ULL
#define VMM_FLAG_PWT      0x008ULL
#define VMM_FLAG_PCD      0x010ULL
#define VMM_FLAG_ACCESSED 0x020ULL
#define VMM_FLAG_DIRTY    0x040ULL
#define VMM_FLAG_HUGE     0x080ULL   /* PDE/PDPTE: 2 MiB / 1 GiB page */
#define VMM_FLAG_GLOBAL   0x100ULL
#define VMM_FLAG_NX       (1ULL << 63)

/* Physical-address mask in a PTE (bits 12..51). */
#define PTE_ADDR_MASK     0x000FFFFFFFFFF000ULL

/* All page-table frames and per-process user frames must come from
   the first 64 MiB of physical RAM (the kernel direct-map region).
   pmm_alloc_frame returns low-first and vmm_* panics if a frame
   above this line sneaks through. This is the x86_64 analogue of
   the i386 "first 16 MiB" invariant — just with 4× headroom since
   4-level page tables are heavier. */
#define KERNEL_DIRECT_MAP_LIMIT 0x04000000ULL   /* 64 MiB */

/* Higher-half kernel image base. Must match linker.ld KERNEL_VMA
   and src/boot/boot.s PDPT_HIGH[510] → PD[0..511]. */
#define KERNEL_VMA_BASE   0xFFFFFFFF80000000ULL

/* Kernel HHDM (higher-half direct-map) base. vmm_init installs a
   physmap of phys [0, 1 GiB) at this VA so kernel code can reach
   any physical page by VA = HHDM_BASE + phys, regardless of which
   per-process PML4 is live. Lives in PML4[256] so every user PML4
   inherits it via the shared kernel half (PML4[256..511]). */
#define KERNEL_HHDM_BASE  0xFFFF800000000000ULL

/* Convert a direct-map physical address to a kernel-virtual pointer. */
static inline void *phys_to_virt_low(uint64_t phys) {
    return (void *)(uintptr_t)(phys + KERNEL_HHDM_BASE);
}

void      vmm_init(void);

void      vmm_map_in(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags);
void      vmm_unmap_in(uint64_t *pml4, uint64_t virt);
uint64_t  vmm_get_physical_in(uint64_t *pml4, uint64_t virt);
int       vmm_set_flags_in(uint64_t *pml4, uint64_t virt, uint64_t flags);

void      vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags);
void      vmm_unmap_page(uint64_t virt);
uint64_t  vmm_get_physical(uint64_t virt);

uint64_t *vmm_kernel_pml4(void);
uint64_t *vmm_new_pml4(void);
void      vmm_free_pml4(uint64_t *pml4);
void      vmm_switch_pml4(uint64_t *pml4);

/* Strip user mappings (PML4[0..255]) from the given table, freeing
   every 4 KiB user frame and the intermediate tables they sit in.
   The kernel half (PML4[256..511]) is untouched. Used by execve
   to drop the old image. */
void      vmm_unmap_user_space(uint64_t *pml4);

/* User-pointer safety helpers. */
int       user_ptr_ok(const void *ptr, uint64_t len, int write);
int32_t   copy_from_user(void *kdst, const void *usrc, uint64_t n);
int32_t   copy_to_user(void *udst, const void *ksrc, uint64_t n);
int32_t   strncpy_from_user(char *kdst, const char *usrc, uint64_t max);

#endif
