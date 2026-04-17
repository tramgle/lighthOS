/* Physical memory manager (x86_64).
 *
 * The pmm is a per-frame bitmap. Frames with bit=1 are allocated;
 * bit=0 is free. pmm_alloc_frame scans low-to-high so page-table
 * and kernel-direct allocations stay within the first KERNEL_DIRECT_-
 * MAP_LIMIT bytes (checked in vmm).
 *
 * The bitmap lives just past the kernel image in physical RAM. We
 * access it through the low-half identity map set up by boot.s
 * (which covers phys 0..1 GiB). Kernel statics live at VMA
 * 0xFFFFFFFF80xxxxxx; boot.s also maps those to the same physical
 * frames. `_kernel_phys_end` from linker.ld gives us the physical
 * end of the image.
 */

#include "mm/pmm.h"
#include "mm/vmm.h"
#include "lib/string.h"
#include "lib/kprintf.h"

extern char _kernel_phys_end;
extern char _kernel_end;

static uint32_t *bitmap;           /* accessed via low-half identity map */
static uint32_t  bitmap_words;     /* number of uint32_t entries */
static uint32_t  total_frames;
static uint32_t  used_frames;

static inline void bitmap_set(uint32_t frame) {
    bitmap[frame / 32] |= (1u << (frame % 32));
}

static inline void bitmap_clear(uint32_t frame) {
    bitmap[frame / 32] &= ~(1u << (frame % 32));
}

static inline bool bitmap_test(uint32_t frame) {
    return (bitmap[frame / 32] >> (frame % 32)) & 1;
}

void pmm_init(multiboot_info_t *mbi) {
    /* Find total usable physical memory. Multiboot1 mmap entries
       describe regions with 64-bit addresses/lengths, so we can
       handle RAM above 4 GiB if QEMU/GRUB provided it. */
    uint64_t mem_end = 0;

    if (!(mbi->flags & (1u << 6))) {
        mem_end = ((uint64_t)mbi->mem_upper + 1024) * 1024;
    } else {
        uint64_t mmap_addr = (uint64_t)mbi->mmap_addr;
        uint64_t mmap_end  = mmap_addr + mbi->mmap_length;

        while (mmap_addr < mmap_end) {
            multiboot_mmap_entry_t *entry =
                (multiboot_mmap_entry_t *)(uintptr_t)mmap_addr;
            if (entry->type == MULTIBOOT_MMAP_AVAILABLE) {
                uint64_t region_end = entry->addr + entry->len;
                if (region_end > mem_end) mem_end = region_end;
            }
            mmap_addr += entry->size + sizeof(entry->size);
        }
    }

    /* Cap to 4 GiB for now; the boot-time identity map only covers
       the first 1 GiB, and honouring anything above 4 GiB would
       need HHDM plumbing the L2 port doesn't have yet. */
    if (mem_end > 0x100000000ULL) mem_end = 0x100000000ULL;

    total_frames = (uint32_t)(mem_end / PAGE_SIZE);
    bitmap_words = (total_frames + 31) / 32;

    /* Place the bitmap just past the kernel image, page-aligned.
       `_kernel_phys_end` is the PHYSICAL end of the image; we access
       it via the identity map. */
    uint64_t kernel_end_phys = (uint64_t)(uintptr_t)&_kernel_phys_end;
    uint64_t bitmap_phys = (kernel_end_phys + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    /* Access the bitmap through the kernel HHDM so it remains
       reachable when CR3 is pointed at a per-process PML4. */
    bitmap = (uint32_t *)(uintptr_t)(bitmap_phys + 0xFFFF800000000000ULL);

    /* Start by marking every frame as used, then carve out the
       availables. */
    memset(bitmap, 0xFF, bitmap_words * sizeof(uint32_t));
    used_frames = total_frames;

    if (mbi->flags & (1u << 6)) {
        uint64_t mmap_addr = (uint64_t)mbi->mmap_addr;
        uint64_t mmap_end  = mmap_addr + mbi->mmap_length;

        while (mmap_addr < mmap_end) {
            multiboot_mmap_entry_t *entry =
                (multiboot_mmap_entry_t *)(uintptr_t)mmap_addr;
            if (entry->type == MULTIBOOT_MMAP_AVAILABLE) {
                uint64_t start = entry->addr;
                uint64_t end   = start + entry->len;
                start = (start + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
                end   = end & ~(uint64_t)(PAGE_SIZE - 1);
                for (uint64_t addr = start; addr < end; addr += PAGE_SIZE) {
                    uint32_t frame = (uint32_t)(addr / PAGE_SIZE);
                    if (frame < total_frames && bitmap_test(frame)) {
                        bitmap_clear(frame);
                        used_frames--;
                    }
                }
            }
            mmap_addr += entry->size + sizeof(entry->size);
        }
    } else {
        for (uint32_t frame = 0; frame < total_frames; frame++) {
            if (bitmap_test(frame)) {
                bitmap_clear(frame);
                used_frames--;
            }
        }
    }

    /* Reserve first 1 MiB (legacy BIOS + low RAM). */
    for (uint32_t i = 0; i < (0x100000 / PAGE_SIZE); i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            used_frames++;
        }
    }

    /* Reserve kernel image + bitmap. */
    uint32_t kernel_start_frame = 0x100000 / PAGE_SIZE;
    uint64_t bitmap_end_phys = bitmap_phys + (uint64_t)bitmap_words * sizeof(uint32_t);
    uint32_t bitmap_end_frame =
        (uint32_t)((bitmap_end_phys + PAGE_SIZE - 1) / PAGE_SIZE);

    for (uint32_t i = kernel_start_frame; i < bitmap_end_frame; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            used_frames++;
        }
    }

    /* Reserve multiboot modules so pmm_alloc_frame doesn't hand out
       pages GRUB already populated with module data. */
    if ((mbi->flags & (1u << 3)) && mbi->mods_count > 0) {
        multiboot_mod_t *mods = (multiboot_mod_t *)(uintptr_t)mbi->mods_addr;
        for (uint32_t i = 0; i < mbi->mods_count; i++) {
            uint64_t start = (uint64_t)mods[i].mod_start;
            uint64_t end   = (uint64_t)mods[i].mod_end;
            /* Reserve the struct array itself too, in case it sits
               in a frame the loop would otherwise hand out. */
            pmm_reserve_range(start, end - start);
        }
        pmm_reserve_range((uint64_t)mbi->mods_addr,
                          (uint64_t)mbi->mods_count * sizeof(multiboot_mod_t));
    }

    serial_printf("[pmm] Total: %u frames (%u MiB). Free: %u. Bitmap @0x%x..0x%x\n",
                  total_frames,
                  (uint32_t)((uint64_t)total_frames * PAGE_SIZE / (1024 * 1024)),
                  total_frames - used_frames,
                  (uint32_t)bitmap_phys, (uint32_t)bitmap_end_phys);
}

void pmm_reserve_range(uint64_t start, uint64_t size) {
    uint32_t first = (uint32_t)(start / PAGE_SIZE);
    uint32_t last  = (uint32_t)((start + size + PAGE_SIZE - 1) / PAGE_SIZE);
    for (uint32_t i = first; i < last && i < total_frames; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            used_frames++;
        }
    }
}

uint64_t pmm_alloc_frame(void) {
    for (uint32_t i = 0; i < bitmap_words; i++) {
        if (bitmap[i] == 0xFFFFFFFFu) continue;
        for (uint32_t bit = 0; bit < 32; bit++) {
            if (!(bitmap[i] & (1u << bit))) {
                uint32_t frame = i * 32 + bit;
                if (frame >= total_frames) return 0;
                bitmap_set(frame);
                used_frames++;
                return (uint64_t)frame * PAGE_SIZE;
            }
        }
    }
    return 0;
}

void pmm_free_frame(uint64_t addr) {
    uint32_t frame = (uint32_t)(addr / PAGE_SIZE);
    if (frame < total_frames && bitmap_test(frame)) {
        bitmap_clear(frame);
        used_frames--;
    }
}

uint32_t pmm_get_free_count(void)  { return total_frames - used_frames; }
uint32_t pmm_get_total_count(void) { return total_frames; }

void pmm_region_iter(void (*cb)(uint32_t start_frame, uint32_t len, bool used)) {
    if (!cb || total_frames == 0) return;
    uint32_t run_start = 0;
    bool     run_used  = bitmap_test(0);
    for (uint32_t i = 1; i < total_frames; i++) {
        bool used = bitmap_test(i);
        if (used != run_used) {
            cb(run_start, i - run_start, run_used);
            run_start = i;
            run_used  = used;
        }
    }
    cb(run_start, total_frames - run_start, run_used);
}
