#ifndef KPRINTF_H
#define KPRINTF_H

#include "include/types.h"

void kprintf(const char *fmt, ...);
void serial_printf(const char *fmt, ...);

#endif
