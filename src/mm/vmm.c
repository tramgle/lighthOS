/* x86_64 4-level paging.
 *
 * Layout after vmm_init:
 *
 *   PML4[0]      → identity map of phys [0, 1 GiB), huge pages.
 *                  Lets the kernel dereference any physical address
 *                  in the direct-map region by casting it to a
 *                  pointer — needed because the MMU walks page
 *                  tables by physical address and the kernel needs
 *                  to edit those tables.
 *
 *   PML4[511]    → kernel text/data at 0xFFFFFFFF80000000, phys 0.
 *                  Same 1 GiB backing via PDPT_HIGH[510]→PD with
 *                  huge pages. Mirrors the boot table layout.
 *
 * Per-process PML4s allocate from pmm and copy PML4[256..511] from
 * the kernel PML4 so the higher-half is shared across every task.
 * User space (PML4[0..255]) is private per process.
 *
 * Page-table frames are allocated from the pmm direct-map region
 * (< 64 MiB) and accessed via the identity map. An allocation
 * escaping that region is a panic — same invariant as i386 vmm,
 * scaled up to x86_64.
 */

#include "mm/vmm.h"
#include "mm/pmm.h"
#include "lib/string.h"
#include "lib/kprintf.h"
#include "kernel/panic.h"

/* Task-system forward decls (kept out of the header to avoid a
   circular include between mm/ and kernel/). */
struct task;
extern struct task  *task_current(void);
extern uint64_t     *task_current_pml4(void);

/* The live kernel PML4. Allocated at vmm_init time from pmm so its
   phys == virt (identity-mapped), which is also what CR3 needs. */
static uint64_t *kernel_pml4;
static uint64_t  kernel_pml4_phys;
static uint64_t *current_cr3;

static void check_low(const char *what, uint64_t phys) {
    if (phys >= KERNEL_DIRECT_MAP_LIMIT) {
        kprintf("vmm: %s produced phys 0x%x above direct-map limit\n",
                what, (uint32_t)phys);
        panic("vmm: frame allocation escaped direct-map region");
    }
}

/* Index extractors for the four paging levels. */
static inline uint32_t pml4_idx(uint64_t va) { return (va >> 39) & 0x1FF; }
static inline uint32_t pdpt_idx(uint64_t va) { return (va >> 30) & 0x1FF; }
static inline uint32_t pd_idx  (uint64_t va) { return (va >> 21) & 0x1FF; }
static inline uint32_t pt_idx  (uint64_t va) { return (va >> 12) & 0x1FF; }

static inline uint64_t *entry_to_table(uint64_t entry) {
    return (uint64_t *)phys_to_virt_low(entry & PTE_ADDR_MASK);
}

static uint64_t alloc_table(const char *what) {
    uint64_t phys = pmm_alloc_frame();
    if (!phys) {
        kprintf("vmm: out of memory for %s\n", what);
        panic("vmm: pmm_alloc_frame failed");
    }
    check_low(what, phys);
    memset(phys_to_virt_low(phys), 0, PAGE_SIZE);
    return phys;
}

/* Ensure a full PML4→PDPT→PD→PT chain exists for `va`, then return
   a pointer to the PT entry. If `user` is set, the intermediate
   entries have USER bit set so ring 3 can traverse them. */
static uint64_t *walk_create(uint64_t *pml4, uint64_t va, int user) {
    uint64_t mid_flags = VMM_FLAG_PRESENT | VMM_FLAG_WRITE;
    if (user) mid_flags |= VMM_FLAG_USER;

    uint64_t *pml4e = &pml4[pml4_idx(va)];
    if (!(*pml4e & VMM_FLAG_PRESENT)) {
        uint64_t phys = alloc_table("pdpt");
        *pml4e = phys | mid_flags;
    } else if (user) {
        *pml4e |= VMM_FLAG_USER;
    }
    uint64_t *pdpt = entry_to_table(*pml4e);

    uint64_t *pdpte = &pdpt[pdpt_idx(va)];
    if (!(*pdpte & VMM_FLAG_PRESENT)) {
        uint64_t phys = alloc_table("pd");
        *pdpte = phys | mid_flags;
    } else if (user) {
        *pdpte |= VMM_FLAG_USER;
    }
    if (*pdpte & VMM_FLAG_HUGE) return NULL;    /* 1 GiB page */
    uint64_t *pd = entry_to_table(*pdpte);

    uint64_t *pde = &pd[pd_idx(va)];
    if (!(*pde & VMM_FLAG_PRESENT)) {
        uint64_t phys = alloc_table("pt");
        *pde = phys | mid_flags;
    } else if (user) {
        *pde |= VMM_FLAG_USER;
    }
    if (*pde & VMM_FLAG_HUGE) return NULL;      /* 2 MiB page */
    uint64_t *pt = entry_to_table(*pde);

    return &pt[pt_idx(va)];
}

/* Walk without allocating; returns NULL on any missing level. */
static uint64_t *walk_lookup(uint64_t *pml4, uint64_t va) {
    uint64_t e = pml4[pml4_idx(va)];
    if (!(e & VMM_FLAG_PRESENT)) return NULL;
    uint64_t *pdpt = entry_to_table(e);
    e = pdpt[pdpt_idx(va)];
    if (!(e & VMM_FLAG_PRESENT)) return NULL;
    if (e & VMM_FLAG_HUGE) return NULL;
    uint64_t *pd = entry_to_table(e);
    e = pd[pd_idx(va)];
    if (!(e & VMM_FLAG_PRESENT)) return NULL;
    if (e & VMM_FLAG_HUGE) return NULL;
    uint64_t *pt = entry_to_table(e);
    return &pt[pt_idx(va)];
}

/* Install a 2 MiB huge page in `pd_table` at slot `pd_i`. */
static void install_huge_2m(uint64_t *pd_table, uint32_t pd_i,
                            uint64_t phys, uint64_t flags) {
    pd_table[pd_i] = (phys & ~(uint64_t)0x1FFFFF)
                   | flags | VMM_FLAG_PRESENT | VMM_FLAG_HUGE;
}

void vmm_init(void) {
    kernel_pml4_phys = alloc_table("kernel pml4");
    kernel_pml4 = (uint64_t *)phys_to_virt_low(kernel_pml4_phys);

    /* HHDM: PML4[256] → PDPT that maps phys [0, 1 GiB) at VMA
       KERNEL_HHDM_BASE (0xFFFF800000000000). Kernel code uses this
       for any physical-memory access so the kernel keeps working
       after CR3 switches to a user PML4 (user PML4[0..255] is
       private, but [256..511] is shared). */
    uint64_t pdpt_hhdm_phys = alloc_table("pdpt_hhdm");
    uint64_t pd_hhdm_phys   = alloc_table("pd_hhdm");
    uint64_t *pdpt_hhdm = phys_to_virt_low(pdpt_hhdm_phys);
    uint64_t *pd_hhdm   = phys_to_virt_low(pd_hhdm_phys);

    pdpt_hhdm[0] = pd_hhdm_phys | VMM_FLAG_PRESENT | VMM_FLAG_WRITE;
    for (uint32_t i = 0; i < 512; i++) {
        install_huge_2m(pd_hhdm, i,
                        (uint64_t)i * 0x200000ULL,
                        VMM_FLAG_WRITE | VMM_FLAG_GLOBAL);
    }
    kernel_pml4[256] = pdpt_hhdm_phys | VMM_FLAG_PRESENT | VMM_FLAG_WRITE;

    /* PML4[511]: higher-half kernel image at VMA 0xFFFFFFFF80000000.
       Reuses pd_hhdm — same 1 GiB backing, different VA. */
    uint64_t pdpt_high_phys = alloc_table("pdpt_high");
    uint64_t *pdpt_high = phys_to_virt_low(pdpt_high_phys);
    pdpt_high[510] = pd_hhdm_phys | VMM_FLAG_PRESENT | VMM_FLAG_WRITE;
    kernel_pml4[511] = pdpt_high_phys | VMM_FLAG_PRESENT | VMM_FLAG_WRITE;

    /* PML4[0..255] stays zero — reserved for per-process user
       space. Any user PML4 gets its own mappings there via
       vmm_map_in / elf_load. */

    __asm__ volatile ("mov %0, %%cr3" :: "r"(kernel_pml4_phys) : "memory");
    current_cr3 = kernel_pml4;

    serial_printf("[vmm] 4-level paging online. "
                  "kernel_pml4@0x%x HHDM@0xFFFF800000000000 kernel@0xFFFFFFFF80000000\n",
                  (uint32_t)kernel_pml4_phys);
}

uint64_t *vmm_kernel_pml4(void) { return kernel_pml4; }

static uint64_t *default_pml4(void) {
    uint64_t *p = task_current_pml4();
    return p ? p : kernel_pml4;
}

void vmm_map_in(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags) {
    int user = (flags & VMM_FLAG_USER) ? 1 : 0;
    uint64_t *pte = walk_create(pml4, virt, user);
    if (!pte) {
        kprintf("vmm_map_in: cannot map 0x%x..0x%x — huge page in the way\n",
                (uint32_t)(virt >> 32), (uint32_t)virt);
        return;
    }
    *pte = (phys & PTE_ADDR_MASK)
         | (flags & 0xFFF)
         | (flags & VMM_FLAG_NX)
         | VMM_FLAG_PRESENT;

    if (pml4 == current_cr3) {
        __asm__ volatile ("invlpg (%0)" :: "r"(virt) : "memory");
    }
}

void vmm_unmap_in(uint64_t *pml4, uint64_t virt) {
    uint64_t *pte = walk_lookup(pml4, virt);
    if (!pte) return;
    *pte = 0;
    if (pml4 == current_cr3) {
        __asm__ volatile ("invlpg (%0)" :: "r"(virt) : "memory");
    }
}

uint64_t vmm_get_physical_in(uint64_t *pml4, uint64_t virt) {
    uint64_t *pte = walk_lookup(pml4, virt);
    if (!pte || !(*pte & VMM_FLAG_PRESENT)) return 0;
    return (*pte & PTE_ADDR_MASK) | (virt & 0xFFF);
}

int vmm_set_flags_in(uint64_t *pml4, uint64_t virt, uint64_t flags) {
    uint64_t *pte = walk_lookup(pml4, virt);
    if (!pte || !(*pte & VMM_FLAG_PRESENT)) return -1;
    *pte = (*pte & PTE_ADDR_MASK)
         | (flags & 0xFFF)
         | (flags & VMM_FLAG_NX)
         | VMM_FLAG_PRESENT;
    if (pml4 == current_cr3) {
        __asm__ volatile ("invlpg (%0)" :: "r"(virt) : "memory");
    }
    return 0;
}

void     vmm_map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    vmm_map_in(default_pml4(), virt, phys, flags);
}

void     vmm_unmap_page(uint64_t virt) {
    vmm_unmap_in(default_pml4(), virt);
}

uint64_t vmm_get_physical(uint64_t virt) {
    return vmm_get_physical_in(default_pml4(), virt);
}

uint64_t *vmm_new_pml4(void) {
    uint64_t phys = pmm_alloc_frame();
    if (!phys) { kprintf("vmm_new_pml4: OOM\n"); return NULL; }
    check_low("new pml4", phys);
    uint64_t *p = phys_to_virt_low(phys);
    memset(p, 0, PAGE_SIZE);
    /* Inherit kernel half (PML4[256..511]) by copy of entries.
       Sharing at this level means every process sees the same
       identity map + higher-half kernel; user space is private. */
    for (int i = 256; i < 512; i++) {
        p[i] = kernel_pml4[i];
    }
    return p;
}

static void free_user_subtree(uint64_t *pml4) {
    for (int i4 = 0; i4 < 256; i4++) {
        uint64_t e4 = pml4[i4];
        if (!(e4 & VMM_FLAG_PRESENT)) continue;
        uint64_t *pdpt = entry_to_table(e4);
        for (int i3 = 0; i3 < 512; i3++) {
            uint64_t e3 = pdpt[i3];
            if (!(e3 & VMM_FLAG_PRESENT)) continue;
            if (e3 & VMM_FLAG_HUGE) { pmm_free_frame(e3 & PTE_ADDR_MASK); continue; }
            uint64_t *pd = entry_to_table(e3);
            for (int i2 = 0; i2 < 512; i2++) {
                uint64_t e2 = pd[i2];
                if (!(e2 & VMM_FLAG_PRESENT)) continue;
                if (e2 & VMM_FLAG_HUGE) { pmm_free_frame(e2 & PTE_ADDR_MASK); continue; }
                uint64_t *pt = entry_to_table(e2);
                for (int i1 = 0; i1 < 512; i1++) {
                    uint64_t e1 = pt[i1];
                    if (e1 & VMM_FLAG_PRESENT) pmm_free_frame(e1 & PTE_ADDR_MASK);
                }
                pmm_free_frame(e2 & PTE_ADDR_MASK);
            }
            pmm_free_frame(e3 & PTE_ADDR_MASK);
        }
        pmm_free_frame(e4 & PTE_ADDR_MASK);
        pml4[i4] = 0;
    }
}

/* Convert a HHDM kernel-virtual pointer back to its physical
   address. Every PML4/PDPT/PD/PT pointer in this module is such:
   pmm_alloc_frame returns phys, and we access it as phys_to_virt_low
   (HHDM VA). */
static inline uint64_t vka_to_phys(const void *p) {
    return (uint64_t)(uintptr_t)p - KERNEL_HHDM_BASE;
}

void vmm_free_pml4(uint64_t *pml4) {
    if (!pml4 || pml4 == kernel_pml4) return;
    free_user_subtree(pml4);
    pmm_free_frame(vka_to_phys(pml4));
}

void vmm_unmap_user_space(uint64_t *pml4) {
    if (!pml4) return;
    free_user_subtree(pml4);
    if (pml4 == current_cr3) {
        __asm__ volatile ("mov %0, %%cr3" :: "r"(vka_to_phys(pml4)) : "memory");
    }
}

void vmm_switch_pml4(uint64_t *pml4) {
    if (!pml4 || pml4 == current_cr3) return;
    current_cr3 = pml4;
    __asm__ volatile ("mov %0, %%cr3" :: "r"(vka_to_phys(pml4)) : "memory");
}

/* --- User-pointer validation ----------------------------------- */

static int page_user_accessible(uint64_t *pml4, uint64_t va, int write) {
    uint64_t e = pml4[pml4_idx(va)];
    if (!(e & VMM_FLAG_PRESENT) || !(e & VMM_FLAG_USER)) return 0;
    uint64_t *pdpt = entry_to_table(e);
    e = pdpt[pdpt_idx(va)];
    if (!(e & VMM_FLAG_PRESENT) || !(e & VMM_FLAG_USER)) return 0;
    if (e & VMM_FLAG_HUGE) {
        if (write && !(e & VMM_FLAG_WRITE)) return 0;
        return 1;
    }
    uint64_t *pd = entry_to_table(e);
    e = pd[pd_idx(va)];
    if (!(e & VMM_FLAG_PRESENT) || !(e & VMM_FLAG_USER)) return 0;
    if (e & VMM_FLAG_HUGE) {
        if (write && !(e & VMM_FLAG_WRITE)) return 0;
        return 1;
    }
    uint64_t *pt = entry_to_table(e);
    e = pt[pt_idx(va)];
    if (!(e & VMM_FLAG_PRESENT) || !(e & VMM_FLAG_USER)) return 0;
    if (write && !(e & VMM_FLAG_WRITE)) return 0;
    return 1;
}

int user_ptr_ok(const void *ptr, uint64_t len, int write) {
    if (len == 0) return 1;
    uint64_t *pml4 = default_pml4();
    if (!pml4) return 0;
    uint64_t start = (uint64_t)(uintptr_t)ptr;
    if (start + len < start) return 0;
    uint64_t first = start & ~(uint64_t)0xFFF;
    uint64_t last  = (start + len - 1) & ~(uint64_t)0xFFF;
    for (uint64_t va = first; ; va += PAGE_SIZE) {
        if (!page_user_accessible(pml4, va, write)) return 0;
        if (va == last) break;
    }
    return 1;
}

int32_t copy_from_user(void *kdst, const void *usrc, uint64_t n) {
    if (!user_ptr_ok(usrc, n, 0)) return -1;
    memcpy(kdst, usrc, (uint32_t)n);
    return (int32_t)n;
}

int32_t copy_to_user(void *udst, const void *ksrc, uint64_t n) {
    if (!user_ptr_ok(udst, n, 1)) return -1;
    memcpy(udst, ksrc, (uint32_t)n);
    return (int32_t)n;
}

int32_t strncpy_from_user(char *kdst, const char *usrc, uint64_t max) {
    if (max == 0) return -1;
    uint64_t *pml4 = default_pml4();
    if (!pml4) return -1;
    uint64_t i = 0;
    uint64_t last_page_checked = (uint64_t)-1;
    while (i < max - 1) {
        uint64_t va = (uint64_t)(uintptr_t)usrc + i;
        uint64_t page = va & ~(uint64_t)0xFFF;
        if (page != last_page_checked) {
            if (!page_user_accessible(pml4, page, 0)) return -1;
            last_page_checked = page;
        }
        char c = *((const char *)(uintptr_t)va);
        kdst[i] = c;
        if (c == '\0') return (int32_t)i;
        i++;
    }
    kdst[max - 1] = '\0';
    return -1;
}
