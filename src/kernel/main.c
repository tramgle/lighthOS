/*
 * L2 kernel_main — exercises the ported pmm + vmm.
 *
 * Initialises pmm from the multiboot memory map, swaps the boot
 * trampoline page tables for a freshly-built kernel PML4 with a
 * 1 GiB identity map + higher-half kernel map, then runs a quick
 * self-test: allocate a frame, map it into user-space virtual
 * range, write a sentinel, read it back, unmap, confirm the
 * mapping is gone. Prints results on COM1 and halts.
 */

#include "include/types.h"
#include "include/multiboot.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "mm/pmm.h"
#include "mm/vmm.h"

/* Port-era task-system stubs. vmm.c references these for
   default_pml4() + user_ptr_ok(); until L4 they return NULL so
   vmm falls back to the kernel PML4. */
struct task;
struct task *task_current(void) { return (void *)0; }
uint64_t *task_current_pml4(void) { return (uint64_t *)0; }

extern void serial_init(void);

static void l2_self_test(void) {
    serial_printf("\n[l2] self-test: pmm alloc + vmm map/write/read/unmap\n");

    uint64_t frame = pmm_alloc_frame();
    serial_printf("[l2]   pmm_alloc_frame -> 0x%lx\n", (uint64_t)frame);
    if (frame == 0) { serial_printf("[l2]   FAIL: no frame\n"); return; }

    /* Pick a canonical low-half VA well beyond the 1 GiB identity
       map so walk_create must allocate fresh PDPT/PD/PT. The
       identity map uses 2 MiB huge pages in PML4[0]→PDPT[0]→PD[*];
       VA 0x40000000 lands at PDPT[1] where the slot is empty. */
    uint64_t va = 0x0000000040000000ULL;
    vmm_map_page(va, frame, VMM_FLAG_WRITE | VMM_FLAG_USER);

    volatile uint64_t *p = (volatile uint64_t *)(uintptr_t)va;
    p[0] = 0xDEADBEEFCAFEBABEULL;
    p[1] = 0x0123456789ABCDEFULL;

    uint64_t phys = vmm_get_physical(va);
    serial_printf("[l2]   vmm_get_physical(0x%lx) = 0x%lx (expected 0x%lx)\n",
                  va, phys, frame);
    serial_printf("[l2]   read-back: 0x%lx 0x%lx\n", p[0], p[1]);

    vmm_unmap_page(va);
    uint64_t phys2 = vmm_get_physical(va);
    serial_printf("[l2]   after unmap, vmm_get_physical = 0x%lx (expected 0)\n", phys2);

    pmm_free_frame(frame);

    if (phys == frame && p[0] == 0 /* unreachable once unmapped */) {
        /* The read after unmap would fault; we never reach here. */
    }
    serial_printf("[l2]   free frames remaining: %u / %u\n",
                  pmm_get_free_count(), pmm_get_total_count());
    serial_printf("[l2] self-test complete\n");
}

void kernel_main(uint32_t magic, multiboot_info_t *mbi) {
    serial_init();

    serial_printf("\n================================\n");
    serial_printf("LighthOS L2: 4-level paging\n");
    serial_printf("================================\n");
    serial_printf("  multiboot magic: 0x%x\n", magic);
    serial_printf("  mbi phys       : 0x%lx\n", (uint64_t)(uintptr_t)mbi);

    if (magic != MULTIBOOT_MAGIC) {
        serial_printf("  bad multiboot magic; halting.\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    pmm_init(mbi);
    vmm_init();

    l2_self_test();

    serial_printf("\nhalting.\n");
    for (;;) __asm__ volatile ("cli; hlt");
}
