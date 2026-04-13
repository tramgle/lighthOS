#include "mm/heap.h"
#include "lib/string.h"
#include "lib/kprintf.h"

typedef struct heap_block {
    uint32_t size;          /* size of data area (not including header) */
    bool     is_free;
    struct heap_block *next;
} heap_block_t;

#define HEADER_SIZE (sizeof(heap_block_t))
#define ALIGN16(x) (((x) + 15) & ~15)

static heap_block_t *heap_start;
static uint32_t      heap_total;

void heap_init(uint32_t start, uint32_t size) {
    /* Align start to 16 bytes */
    start = ALIGN16(start);
    heap_start = (heap_block_t *)start;
    heap_total = size;

    heap_start->size    = size - HEADER_SIZE;
    heap_start->is_free = true;
    heap_start->next    = NULL;

    serial_printf("[heap] Initialized at 0x%x, %u bytes\n", start, size);
}

void *kmalloc(uint32_t size) {
    if (size == 0) return NULL;
    size = ALIGN16(size);

    heap_block_t *block = heap_start;
    while (block) {
        if (block->is_free && block->size >= size) {
            /* Split if there's enough room for another block */
            if (block->size >= size + HEADER_SIZE + 16) {
                heap_block_t *new_block = (heap_block_t *)((uint8_t *)block + HEADER_SIZE + size);
                new_block->size    = block->size - size - HEADER_SIZE;
                new_block->is_free = true;
                new_block->next    = block->next;
                block->size = size;
                block->next = new_block;
            }
            block->is_free = false;
            return (void *)((uint8_t *)block + HEADER_SIZE);
        }
        block = block->next;
    }

    kprintf("kmalloc: out of memory (requested %u bytes)\n", size);
    return NULL;
}

void kfree(void *ptr) {
    if (!ptr) return;

    heap_block_t *block = (heap_block_t *)((uint8_t *)ptr - HEADER_SIZE);
    block->is_free = true;

    /* Coalesce with next block if it's free */
    if (block->next && block->next->is_free) {
        block->size += HEADER_SIZE + block->next->size;
        block->next = block->next->next;
    }

    /* Coalesce with previous block if it's free */
    heap_block_t *prev = heap_start;
    if (prev != block) {
        while (prev && prev->next != block) {
            prev = prev->next;
        }
        if (prev && prev->is_free) {
            prev->size += HEADER_SIZE + block->size;
            prev->next = block->next;
        }
    }
}

uint32_t heap_get_used(void) {
    uint32_t used = 0;
    heap_block_t *block = heap_start;
    while (block) {
        if (!block->is_free) used += block->size + HEADER_SIZE;
        block = block->next;
    }
    return used;
}

uint32_t heap_get_free(void) {
    uint32_t free_bytes = 0;
    heap_block_t *block = heap_start;
    while (block) {
        if (block->is_free) free_bytes += block->size;
        block = block->next;
    }
    return free_bytes;
}
