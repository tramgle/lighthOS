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

/* Toggle the kernel's cooked-mode line discipline. In raw mode the
   driver passes bytes through unchanged: no echo, no BS handling, no
   \r→\n normalization skip. Ctrl-C / Ctrl-Z still route to the
   foreground pgid — matches the POSIX ISIG-on-in-raw default. */
void serial_set_raw(int enable);
int  serial_get_raw(void);

#endif
