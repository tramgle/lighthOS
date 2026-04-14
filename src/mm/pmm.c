#include "mm/pmm.h"
#include "lib/string.h"
#include "lib/kprintf.h"

extern uint32_t _kernel_end;

static uint32_t *bitmap;
static uint32_t bitmap_size;    /* in uint32_t entries */
static uint32_t total_frames;
static uint32_t used_frames;

static inline void bitmap_set(uint32_t frame) {
    bitmap[frame / 32] |= (1 << (frame % 32));
}

static inline void bitmap_clear(uint32_t frame) {
    bitmap[frame / 32] &= ~(1 << (frame % 32));
}

static inline bool bitmap_test(uint32_t frame) {
    return bitmap[frame / 32] & (1 << (frame % 32));
}

void pmm_init(multiboot_info_t *mbi) {
    /* Find total usable memory from multiboot memory map */
    uint32_t mem_end = 0;

    if (!(mbi->flags & (1 << 6))) {
        /* No memory map available, fall back to mem_upper */
        mem_end = (mbi->mem_upper + 1024) * 1024;  /* convert KB to bytes */
    } else {
        uint32_t mmap_addr = mbi->mmap_addr;
        uint32_t mmap_end  = mmap_addr + mbi->mmap_length;

        while (mmap_addr < mmap_end) {
            multiboot_mmap_entry_t *entry = (multiboot_mmap_entry_t *)mmap_addr;
            if (entry->type == MULTIBOOT_MMAP_AVAILABLE) {
                uint32_t region_end = (uint32_t)(entry->addr + entry->len);
                if (region_end > mem_end) {
                    mem_end = region_end;
                }
            }
            mmap_addr += entry->size + sizeof(entry->size);
        }
    }

    total_frames = mem_end / PAGE_SIZE;
    bitmap_size  = (total_frames + 31) / 32;

    /* Place bitmap right after kernel, page-aligned */
    bitmap = (uint32_t *)(((uint32_t)&_kernel_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
    memset(bitmap, 0xFF, bitmap_size * sizeof(uint32_t));  /* mark all as used */
    used_frames = total_frames;

    /* Free frames based on memory map */
    if (mbi->flags & (1 << 6)) {
        uint32_t mmap_addr = mbi->mmap_addr;
        uint32_t mmap_end  = mmap_addr + mbi->mmap_length;

        while (mmap_addr < mmap_end) {
            multiboot_mmap_entry_t *entry = (multiboot_mmap_entry_t *)mmap_addr;
            if (entry->type == MULTIBOOT_MMAP_AVAILABLE) {
                uint32_t start = (uint32_t)entry->addr;
                uint32_t end   = start + (uint32_t)entry->len;
                /* Align start up, end down to page boundaries */
                start = (start + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
                end   = end & ~(PAGE_SIZE - 1);
                for (uint32_t addr = start; addr < end; addr += PAGE_SIZE) {
                    uint32_t frame = addr / PAGE_SIZE;
                    if (frame < total_frames) {
                        bitmap_clear(frame);
                        used_frames--;
                    }
                }
            }
            mmap_addr += entry->size + sizeof(entry->size);
        }
    } else {
        /* No mmap — assume all frames in [0, mem_end) are available.
           The reserve loops below mark the low 1MB and the kernel/bitmap
           region as used again. */
        for (uint32_t frame = 0; frame < total_frames; frame++) {
            bitmap_clear(frame);
            used_frames--;
        }
    }

    /* Reserve first 1MB (BIOS, VGA, etc.) */
    for (uint32_t i = 0; i < (0x100000 / PAGE_SIZE); i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            used_frames++;
        }
    }

    /* Reserve kernel + bitmap region */
    uint32_t kernel_start_frame = 0x100000 / PAGE_SIZE;  /* kernel loaded at 1MB */
    uint32_t bitmap_end = (uint32_t)bitmap + bitmap_size * sizeof(uint32_t);
    uint32_t bitmap_end_frame = (bitmap_end + PAGE_SIZE - 1) / PAGE_SIZE;

    for (uint32_t i = kernel_start_frame; i < bitmap_end_frame; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            used_frames++;
        }
    }

    serial_printf("[pmm] Total: %u frames (%uMB), Used: %u, Free: %u\n",
                  total_frames, (total_frames * PAGE_SIZE) / (1024 * 1024),
                  used_frames, total_frames - used_frames);
}

void pmm_reserve_range(uint32_t start, uint32_t size) {
    uint32_t first = start / PAGE_SIZE;
    uint32_t last  = (start + size + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint32_t i = first; i < last && i < total_frames; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            used_frames++;
        }
    }
}

uint32_t pmm_alloc_frame(void) {
    for (uint32_t i = 0; i < bitmap_size; i++) {
        if (bitmap[i] == 0xFFFFFFFF) continue;  /* all used in this group */
        for (uint32_t bit = 0; bit < 32; bit++) {
            if (!(bitmap[i] & (1 << bit))) {
                uint32_t frame = i * 32 + bit;
                if (frame >= total_frames) return 0;
                bitmap_set(frame);
                used_frames++;
                return frame * PAGE_SIZE;
            }
        }
    }
    return 0;  /* out of memory */
}

void pmm_free_frame(uint32_t addr) {
    uint32_t frame = addr / PAGE_SIZE;
    if (frame < total_frames && bitmap_test(frame)) {
        bitmap_clear(frame);
        used_frames--;
    }
}

uint32_t pmm_get_free_count(void) {
    return total_frames - used_frames;
}

uint32_t pmm_get_total_count(void) {
    return total_frames;
}

void pmm_region_iter(void (*cb)(uint32_t start_frame, uint32_t len, bool used)) {
    if (!cb || total_frames == 0) return;
    uint32_t run_start = 0;
    bool run_used = bitmap_test(0);
    for (uint32_t i = 1; i < total_frames; i++) {
        bool used = bitmap_test(i);
        if (used != run_used) {
            cb(run_start, i - run_start, run_used);
            run_start = i;
            run_used = used;
        }
    }
    cb(run_start, total_frames - run_start, run_used);
}
