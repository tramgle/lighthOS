#ifndef HEAP_H
#define HEAP_H

#include "include/types.h"

void  heap_init(uint32_t start, uint32_t size);
void *kmalloc(uint32_t size);
void  kfree(void *ptr);

uint32_t heap_get_used(void);
uint32_t heap_get_free(void);

#endif
