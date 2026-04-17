#ifndef CONSOLE_H
#define CONSOLE_H

#include "include/types.h"

void    console_init(void);
ssize_t console_read(void *buf, size_t count);
ssize_t console_write(const void *buf, size_t count);
/* 0 = nothing read yet, 1 = last byte came from serial, 2 = keyboard. */
int     console_last_input_src(void);

#endif
