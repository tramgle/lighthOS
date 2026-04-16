#ifndef KPRINTF_H
#define KPRINTF_H

#include "include/types.h"

void kprintf(const char *fmt, ...);
void serial_printf(const char *fmt, ...);

/* Boot log: an in-memory linear buffer that captures every character
   kputchar writes while enabled. Intended use:
     1. Call boot_log_enable() right after serial_init() so early boot
        messages get captured.
     2. Once the root fs is mounted, call boot_log_flush(path) to dump
        the captured buffer to a file on a writable fs.
   The buffer is fixed-size; overflow is silently dropped (truncated
   write). boot_log_flush keeps appending new output to the file — it
   does not disable the buffer after flushing, so a second flush later
   will rewrite the file from scratch with the full captured log. */
void boot_log_enable(void);
void boot_log_flush(const char *path);

#endif
