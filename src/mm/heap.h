#ifndef HEAP_H
#define HEAP_H

#include "include/types.h"

void  heap_init(uint64_t start, uint64_t size);
void *kmalloc(uint64_t size);
void  kfree(void *ptr);

uint64_t heap_get_used(void);
uint64_t heap_get_free(void);

#endif
