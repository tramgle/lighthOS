#ifndef VMM_H
#define VMM_H

#include "include/types.h"

#define VMM_FLAG_PRESENT  0x01
#define VMM_FLAG_WRITE    0x02
#define VMM_FLAG_USER     0x04

/* Invariant: every page directory, page table, and user frame must come
   from the first 16MB of physical RAM. That's the only region the kernel
   can touch directly (identity-mapped at boot). pmm_alloc_frame hands
   out low frames first so this is normally satisfied; vmm_new_pd /
   vmm_map_in panic if a non-low frame ever sneaks through. */

void     vmm_init(void);

/* Operations on an explicit page directory (caller supplies the virt
   address of the PD, which is also its phys address since PDs live in
   the identity-mapped region). The default wrappers below forward to
   the current task's PD. */
void     vmm_map_in(uint32_t *pd, uint32_t virt, uint32_t phys, uint32_t flags);
void     vmm_unmap_in(uint32_t *pd, uint32_t virt);
uint32_t vmm_get_physical_in(uint32_t *pd, uint32_t virt);

/* Convenience wrappers that operate on the current task's PD (or the
   kernel PD if tasks haven't been initialised yet). */
void     vmm_map_page(uint32_t virt, uint32_t phys, uint32_t flags);
void     vmm_unmap_page(uint32_t virt);
uint32_t vmm_get_physical(uint32_t virt);

/* Page-directory management. */
uint32_t *vmm_kernel_pd(void);
uint32_t *vmm_new_pd(void);
void      vmm_free_pd(uint32_t *pd);
void      vmm_switch_pd(uint32_t *pd);

/* Strip all user mappings from `pd` (PDEs 4..1023): free each user frame,
   free the page table, zero the PDE. Kernel PDEs 0..3 are untouched.
   Used by execve to drop the old image before loading the new one. */
void      vmm_unmap_user_space(uint32_t *pd);

/* --- User-pointer safety helpers ---
   Syscall handlers receive raw user pointers via registers. Before any
   dereference, validate that the full range is present and has the
   USER bit set in the page tables; for writes also require WRITE.
   `user_ptr_ok` returns 1 on success, 0 on any gap. Passing write=1
   additionally requires the PTE W bit.
   The copy helpers do the validation themselves page-by-page and
   return -1 on any unreachable byte. */
int       user_ptr_ok(const void *ptr, uint32_t len, int write);
int32_t   copy_from_user(void *kdst, const void *usrc, uint32_t n);
int32_t   copy_to_user(void *udst, const void *ksrc, uint32_t n);
/* Copy a NUL-terminated string from user space, stopping at NUL or
   at `max - 1` bytes. Guarantees a NUL-terminated kdst on success.
   Returns length (not including NUL) on success, -1 on fault or if
   the string is longer than `max`. */
int32_t   strncpy_from_user(char *kdst, const char *usrc, uint32_t max);

#endif
