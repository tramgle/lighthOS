#include "mm/heap.h"
#include "lib/string.h"
#include "lib/kprintf.h"

typedef struct heap_block {
    uint64_t size;
    bool     is_free;
    struct heap_block *next;
} heap_block_t;

#define HEADER_SIZE (sizeof(heap_block_t))
#define ALIGN16(x) (((x) + 15) & ~(uint64_t)15)

static heap_block_t *heap_start;
static uint64_t      heap_total;

void heap_init(uint64_t start, uint64_t size) {
    start = ALIGN16(start);
    heap_start = (heap_block_t *)(uintptr_t)start;
    heap_total = size;
    heap_start->size    = size - HEADER_SIZE;
    heap_start->is_free = true;
    heap_start->next    = 0;
    serial_printf("[heap] initialised at 0x%lx (%lu bytes)\n", start, size);
}

void *kmalloc(uint64_t size) {
    if (size == 0) return 0;
    size = ALIGN16(size);

    heap_block_t *block = heap_start;
    while (block) {
        if (block->is_free && block->size >= size) {
            if (block->size >= size + HEADER_SIZE + 16) {
                heap_block_t *nb = (heap_block_t *)((uint8_t *)block + HEADER_SIZE + size);
                nb->size    = block->size - size - HEADER_SIZE;
                nb->is_free = true;
                nb->next    = block->next;
                block->size = size;
                block->next = nb;
            }
            block->is_free = false;
            return (uint8_t *)block + HEADER_SIZE;
        }
        block = block->next;
    }
    kprintf("kmalloc: OOM (requested %lu bytes)\n", size);
    return 0;
}

void kfree(void *ptr) {
    if (!ptr) return;
    heap_block_t *block = (heap_block_t *)((uint8_t *)ptr - HEADER_SIZE);
    block->is_free = true;

    if (block->next && block->next->is_free) {
        block->size += HEADER_SIZE + block->next->size;
        block->next  = block->next->next;
    }
    heap_block_t *prev = heap_start;
    if (prev != block) {
        while (prev && prev->next != block) prev = prev->next;
        if (prev && prev->is_free) {
            prev->size += HEADER_SIZE + block->size;
            prev->next  = block->next;
        }
    }
}

uint64_t heap_get_used(void) {
    uint64_t u = 0;
    for (heap_block_t *b = heap_start; b; b = b->next)
        if (!b->is_free) u += b->size + HEADER_SIZE;
    return u;
}

uint64_t heap_get_free(void) {
    uint64_t f = 0;
    for (heap_block_t *b = heap_start; b; b = b->next)
        if (b->is_free) f += b->size;
    return f;
}
