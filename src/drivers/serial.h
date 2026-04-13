#ifndef SERIAL_H
#define SERIAL_H

#include "include/types.h"

#define SERIAL_COM1 0x3F8

void serial_init(void);
void serial_init_irq(void);
void serial_putchar(char c);
void serial_puts(const char *s);
char serial_getchar(void);
bool serial_has_data(void);

#endif
