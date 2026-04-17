/* Simple first-fit freelist allocator backed by sys_sbrk.
 *
 * Layout: each block has a 12-byte header { size, prev, next_free } —
 * size is total block size including the header, prev_size lets us walk
 * backwards for coalescing, next_free links free blocks in a singly-
 * linked free list. The allocator doesn't keep a separate "used" list;
 * allocated blocks are tracked implicitly by not appearing in the free
 * list.
 *
 * The arena grows in whole pages (4 KB) via sys_sbrk. On free, adjacent
 * free blocks coalesce in both directions. No thread safety — this is a
 * single-threaded user process.
 */

#include "syscall.h"
#include "ulib.h"
#include <stdint.h>

typedef struct block {
    uint32_t size;          /* total size including this header, bytes */
    uint32_t prev_size;     /* size of physical predecessor, 0 if at arena start */
    struct block *next_free;
    uint32_t free;          /* 0 = allocated, 1 = free */
} block_t;

#define HDR_SIZE     (sizeof(block_t))
#define MIN_BLOCK    (HDR_SIZE + 16)   /* don't split below this */
#define ALIGN        8
#define ALIGN_UP(n)  (((n) + (ALIGN - 1)) & ~(ALIGN - 1))

static block_t *free_head;
static uint8_t *arena_start;   /* inclusive */
static uint8_t *arena_end;     /* exclusive */

static block_t *next_block(block_t *b) {
    uint8_t *p = (uint8_t *)b + b->size;
    return (p < arena_end) ? (block_t *)p : 0;
}

static void freelist_insert(block_t *b) {
    b->free = 1;
    b->next_free = free_head;
    free_head = b;
}

static void freelist_remove(block_t *b) {
    b->free = 0;
    if (free_head == b) {
        free_head = b->next_free;
        return;
    }
    for (block_t *p = free_head; p; p = p->next_free) {
        if (p->next_free == b) {
            p->next_free = b->next_free;
            return;
        }
    }
}

/* Extend the arena by `bytes`, carve a free block covering the new
   region, insert it into the free list, and return it. */
static block_t *grow_arena(uint32_t bytes) {
    uint32_t rounded = (bytes + 4095) & ~4095u;
    long raw = sys_sbrk((long)rounded);
    if (raw == -1) return 0;
    void *old_brk = (void *)(uintptr_t)raw;

    block_t *b = (block_t *)old_brk;
    b->size = rounded;
    b->prev_size = 0;
    b->free = 1;
    b->next_free = 0;

    if (!arena_start) {
        arena_start = (uint8_t *)old_brk;
    } else {
        /* Stitch prev_size into the new block so we can coalesce back. */
        uint8_t *prev_end = arena_end;
        if (prev_end == (uint8_t *)old_brk) {
            /* Physically contiguous — find the last block by walking. */
            block_t *scan = (block_t *)arena_start;
            block_t *last = scan;
            while ((uint8_t *)scan < arena_end) {
                last = scan;
                scan = (block_t *)((uint8_t *)scan + scan->size);
            }
            b->prev_size = last->size;
        }
    }
    arena_end = (uint8_t *)old_brk + rounded;

    freelist_insert(b);
    return b;
}

static void maybe_split(block_t *b, uint32_t needed) {
    if (b->size < needed + MIN_BLOCK) return;
    block_t *tail = (block_t *)((uint8_t *)b + needed);
    tail->size = b->size - needed;
    tail->prev_size = needed;
    tail->free = 1;
    tail->next_free = 0;
    b->size = needed;

    /* Update the following block's prev_size if it exists. */
    block_t *after = next_block(tail);
    if (after) after->prev_size = tail->size;

    freelist_insert(tail);
}

void *malloc(uint32_t nbytes) {
    if (nbytes == 0) return 0;
    uint32_t needed = ALIGN_UP(nbytes + HDR_SIZE);

    /* First-fit scan of the free list. */
    block_t **pp = &free_head;
    for (block_t *b = free_head; b; b = b->next_free) {
        if (b->size >= needed) {
            *pp = b->next_free;
            b->free = 0;
            maybe_split(b, needed);
            return (void *)((uint8_t *)b + HDR_SIZE);
        }
        pp = &b->next_free;
    }

    /* Out of space — grow. */
    block_t *grown = grow_arena(needed);
    if (!grown) return 0;
    /* Recurse once: the new block is in the free list now. */
    return malloc(nbytes);
}

void free(void *ptr) {
    if (!ptr) return;
    block_t *b = (block_t *)((uint8_t *)ptr - HDR_SIZE);
    if (b->free) return;  /* double free — ignore */
    freelist_insert(b);

    /* Coalesce with physical successor if free. */
    block_t *after = next_block(b);
    if (after && after->free) {
        freelist_remove(after);
        b->size += after->size;
        block_t *after2 = next_block(b);
        if (after2) after2->prev_size = b->size;
    }

    /* Coalesce with physical predecessor if free. */
    if (b->prev_size) {
        block_t *prev = (block_t *)((uint8_t *)b - b->prev_size);
        if (prev->free) {
            freelist_remove(b);
            freelist_remove(prev);
            prev->size += b->size;
            block_t *after3 = next_block(prev);
            if (after3) after3->prev_size = prev->size;
            freelist_insert(prev);
        }
    }
}

void *calloc(uint32_t n, uint32_t sz) {
    uint32_t total = n * sz;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *realloc(void *ptr, uint32_t nbytes) {
    if (!ptr) return malloc(nbytes);
    if (nbytes == 0) { free(ptr); return 0; }
    block_t *b = (block_t *)((uint8_t *)ptr - HDR_SIZE);
    uint32_t current_payload = b->size - HDR_SIZE;
    if (nbytes <= current_payload) return ptr;

    void *n = malloc(nbytes);
    if (!n) return 0;
    memcpy(n, ptr, current_payload);
    free(ptr);
    return n;
}
