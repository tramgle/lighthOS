#include "mm/vmm.h"
#include "mm/pmm.h"
#include "lib/string.h"
#include "lib/kprintf.h"
#include "kernel/panic.h"

/* Forward declaration: pulled from task.c without a header dependency so
   mm/ doesn't bleed into kernel/ at the include level. Returns NULL when
   the task system hasn't been initialised yet (e.g. during vmm_init). */
struct task;
extern struct task *task_current(void);
extern uint32_t     *task_current_pd(void);

/* Kernel page directory: 1024 entries. Always present; every other PD
   copies its first four entries verbatim to inherit the identity map. */
static uint32_t page_directory[1024] __attribute__((aligned(4096)));

/* Pre-allocated static page tables for the first 16MB (4 tables × 4MB).
   These are SHARED by every process PD — the kernel map is the same
   everywhere. */
static uint32_t page_tables[4][1024] __attribute__((aligned(4096)));

/* Cached CR3 so we can skip no-op writes. */
static uint32_t *current_cr3;

/* All kernel-addressable memory lives in the first 16MB (the identity-
   mapped region). Allocations above that boundary can't be zeroed or
   read directly by kernel code. Enforce loudly rather than silently
   corrupting state. */
#define KERNEL_DIRECT_MAP_LIMIT 0x01000000u

static void check_low(const char *what, uint32_t frame) {
    if (frame >= KERNEL_DIRECT_MAP_LIMIT) {
        kprintf("vmm: %s produced frame 0x%x above 16MB\n", what, frame);
        panic("vmm: frame allocation escaped identity-mapped region");
    }
}

void vmm_init(void) {
    memset(page_directory, 0, sizeof(page_directory));

    /* Identity map first 16MB using the pre-allocated page tables. */
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 1024; j++) {
            uint32_t phys = (i * 1024 + j) * PAGE_SIZE;
            page_tables[i][j] = phys | VMM_FLAG_PRESENT | VMM_FLAG_WRITE;
        }
        page_directory[i] = (uint32_t)&page_tables[i] | VMM_FLAG_PRESENT | VMM_FLAG_WRITE;
    }

    /* Load page directory into CR3 */
    __asm__ volatile ("mov %0, %%cr3" :: "r"(page_directory));
    current_cr3 = page_directory;

    /* Enable paging */
    uint32_t cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000;
    __asm__ volatile ("mov %0, %%cr0" :: "r"(cr0));

    serial_printf("[vmm] Paging enabled, first 16MB identity-mapped\n");
}

uint32_t *vmm_kernel_pd(void) {
    return page_directory;
}

/* Resolve the PD that kernel callers implicitly target. Before task
   init we use the kernel PD; after, we defer to the running task. */
static uint32_t *default_pd(void) {
    uint32_t *pd = task_current_pd();
    return pd ? pd : page_directory;
}

void vmm_map_in(uint32_t *pd, uint32_t virt, uint32_t phys, uint32_t flags) {
    uint32_t pd_idx = virt >> 22;
    uint32_t pt_idx = (virt >> 12) & 0x3FF;

    if (!(pd[pd_idx] & VMM_FLAG_PRESENT)) {
        uint32_t pt_phys = pmm_alloc_frame();
        if (!pt_phys) {
            kprintf("vmm: out of memory for page table\n");
            return;
        }
        check_low("page-table alloc", pt_phys);
        memset((void *)pt_phys, 0, PAGE_SIZE);
        pd[pd_idx] = pt_phys | VMM_FLAG_PRESENT | VMM_FLAG_WRITE | (flags & VMM_FLAG_USER);
    } else if (flags & VMM_FLAG_USER) {
        pd[pd_idx] |= VMM_FLAG_USER;
    }

    uint32_t *pt = (uint32_t *)(pd[pd_idx] & 0xFFFFF000);
    pt[pt_idx] = (phys & 0xFFFFF000) | (flags & 0xFFF) | VMM_FLAG_PRESENT;

    /* Only invalidate the TLB when the mapping we just wrote is the
       one the CPU will use on the next access. Mapping into another
       process's PD doesn't affect our TLB. */
    if (pd == current_cr3) {
        __asm__ volatile ("invlpg (%0)" :: "r"(virt) : "memory");
    }
}

void vmm_unmap_in(uint32_t *pd, uint32_t virt) {
    uint32_t pd_idx = virt >> 22;
    uint32_t pt_idx = (virt >> 12) & 0x3FF;

    if (!(pd[pd_idx] & VMM_FLAG_PRESENT)) return;

    uint32_t *pt = (uint32_t *)(pd[pd_idx] & 0xFFFFF000);
    pt[pt_idx] = 0;

    if (pd == current_cr3) {
        __asm__ volatile ("invlpg (%0)" :: "r"(virt) : "memory");
    }
}

uint32_t vmm_get_physical_in(uint32_t *pd, uint32_t virt) {
    uint32_t pd_idx = virt >> 22;
    uint32_t pt_idx = (virt >> 12) & 0x3FF;

    if (!(pd[pd_idx] & VMM_FLAG_PRESENT)) return 0;

    uint32_t *pt = (uint32_t *)(pd[pd_idx] & 0xFFFFF000);
    if (!(pt[pt_idx] & VMM_FLAG_PRESENT)) return 0;

    return (pt[pt_idx] & 0xFFFFF000) | (virt & 0xFFF);
}

void vmm_map_page(uint32_t virt, uint32_t phys, uint32_t flags) {
    vmm_map_in(default_pd(), virt, phys, flags);
}

void vmm_unmap_page(uint32_t virt) {
    vmm_unmap_in(default_pd(), virt);
}

uint32_t vmm_get_physical(uint32_t virt) {
    return vmm_get_physical_in(default_pd(), virt);
}

uint32_t *vmm_new_pd(void) {
    uint32_t frame = pmm_alloc_frame();
    if (!frame) {
        kprintf("vmm: out of memory for page directory\n");
        return NULL;
    }
    check_low("pd alloc", frame);
    uint32_t *pd = (uint32_t *)frame;
    memset(pd, 0, PAGE_SIZE);
    /* Share the kernel's static page-tables for the first 16MB so every
       process has the kernel identity-mapped without copying. */
    for (int i = 0; i < 4; i++) {
        pd[i] = page_directory[i];
    }
    return pd;
}

void vmm_free_pd(uint32_t *pd) {
    if (!pd || pd == page_directory) return;
    /* Kernel PDEs (0..3) point at the shared static tables — leave them. */
    for (int i = 4; i < 1024; i++) {
        if (!(pd[i] & VMM_FLAG_PRESENT)) continue;
        uint32_t pt_phys = pd[i] & 0xFFFFF000;
        uint32_t *pt = (uint32_t *)pt_phys;
        for (int j = 0; j < 1024; j++) {
            if (pt[j] & VMM_FLAG_PRESENT) {
                pmm_free_frame(pt[j] & 0xFFFFF000);
            }
        }
        pmm_free_frame(pt_phys);
        pd[i] = 0;
    }
    pmm_free_frame((uint32_t)pd);
}

void vmm_switch_pd(uint32_t *pd) {
    if (!pd || pd == current_cr3) return;
    current_cr3 = pd;
    __asm__ volatile ("mov %0, %%cr3" :: "r"(pd) : "memory");
}

/* --- User-pointer helpers ---
   Walk the current PD page-by-page. An address is "user safe" only if
   its PTE has both PRESENT and USER set (and, for writes, WRITE).
   We do NOT rely on paging to fault-and-recover — the kernel's page
   fault handler panics today, so we must pre-validate. */

static int page_user_accessible(uint32_t *pd, uint32_t va, int write) {
    uint32_t pd_idx = va >> 22;
    uint32_t pt_idx = (va >> 12) & 0x3FF;
    uint32_t pde = pd[pd_idx];
    if (!(pde & VMM_FLAG_PRESENT)) return 0;
    if (!(pde & VMM_FLAG_USER)) return 0;
    uint32_t *pt = (uint32_t *)(pde & 0xFFFFF000);
    uint32_t pte = pt[pt_idx];
    if (!(pte & VMM_FLAG_PRESENT)) return 0;
    if (!(pte & VMM_FLAG_USER)) return 0;
    if (write && !(pte & VMM_FLAG_WRITE)) return 0;
    return 1;
}

int user_ptr_ok(const void *ptr, uint32_t len, int write) {
    if (len == 0) return 1;
    uint32_t *pd = default_pd();
    if (!pd) return 0;
    uint32_t start = (uint32_t)ptr;
    /* Overflow guard: start + len must not wrap. */
    if (start + len < start) return 0;
    uint32_t first_page = start & ~0xFFFu;
    uint32_t last_page  = (start + len - 1) & ~0xFFFu;
    for (uint32_t va = first_page; ; va += PAGE_SIZE) {
        if (!page_user_accessible(pd, va, write)) return 0;
        if (va == last_page) break;
    }
    return 1;
}

int32_t copy_from_user(void *kdst, const void *usrc, uint32_t n) {
    if (!user_ptr_ok(usrc, n, 0)) return -1;
    memcpy(kdst, usrc, n);
    return (int32_t)n;
}

int32_t copy_to_user(void *udst, const void *ksrc, uint32_t n) {
    if (!user_ptr_ok(udst, n, 1)) return -1;
    memcpy(udst, ksrc, n);
    return (int32_t)n;
}

int32_t strncpy_from_user(char *kdst, const char *usrc, uint32_t max) {
    if (max == 0) return -1;
    uint32_t *pd = default_pd();
    if (!pd) return -1;
    uint32_t i = 0;
    uint32_t last_page_checked = 0xFFFFFFFFu;
    while (i < max - 1) {
        uint32_t va = (uint32_t)usrc + i;
        /* Re-check the page when we cross a boundary. */
        uint32_t page = va & ~0xFFFu;
        if (page != last_page_checked) {
            if (!page_user_accessible(pd, page, 0)) return -1;
            last_page_checked = page;
        }
        char c = *((const char *)va);
        kdst[i] = c;
        if (c == '\0') return (int32_t)i;
        i++;
    }
    /* Ran off the end without finding NUL. */
    kdst[max - 1] = '\0';
    return -1;
}

void vmm_unmap_user_space(uint32_t *pd) {
    if (!pd) return;
    for (int i = 4; i < 1024; i++) {
        if (!(pd[i] & VMM_FLAG_PRESENT)) continue;
        uint32_t pt_phys = pd[i] & 0xFFFFF000;
        uint32_t *pt = (uint32_t *)pt_phys;
        for (int j = 0; j < 1024; j++) {
            if (pt[j] & VMM_FLAG_PRESENT) {
                pmm_free_frame(pt[j] & 0xFFFFF000);
            }
        }
        pmm_free_frame(pt_phys);
        pd[i] = 0;
    }
    /* Reload CR3 to flush the TLB if this PD is live. */
    if (pd == current_cr3) {
        __asm__ volatile ("mov %0, %%cr3" :: "r"(pd) : "memory");
    }
}
