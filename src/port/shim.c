/* Port-era shims.
 *
 * Provides serial_printf / kprintf / panic / minimal port I/O until
 * the full drivers/ + lib/kprintf.c + kernel/panic.c can be brought
 * back (they currently pull in vfs, vga, process, isr — none of
 * which are ported yet). Self-contained: only stdarg + string.
 *
 * Deleted once PORT_MINIMAL flips back to 0.
 */

#include "include/types.h"
#include "lib/string.h"
#include "lib/kprintf.h"
#include "kernel/panic.h"

#define COM1 0x3F8

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

void serial_init(void) {
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x03);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
}

static void serial_putc(char c) {
    while ((inb(COM1 + 5) & 0x20) == 0) { }
    outb(COM1, (uint8_t)c);
}

static void sputs(const char *s) {
    for (; *s; s++) {
        if (*s == '\n') serial_putc('\r');
        serial_putc(*s);
    }
}

static void print_u(uint64_t val, int base, int width, char pad) {
    static const char dig[] = "0123456789abcdef";
    char buf[32];
    int i = 0;
    if (val == 0) buf[i++] = '0';
    while (val > 0) { buf[i++] = dig[val % base]; val /= base; }
    while (i < width) buf[i++] = pad;
    while (--i >= 0) serial_putc(buf[i]);
}

static void vprintf_shim(const char *fmt, va_list ap) {
    for (; *fmt; fmt++) {
        if (*fmt != '%') { if (*fmt == '\n') serial_putc('\r'); serial_putc(*fmt); continue; }
        fmt++;
        int width = 0;
        char pad = ' ';
        if (*fmt == '0') { pad = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }
        int is_long = 0;
        if (*fmt == 'l') { is_long = 1; fmt++; if (*fmt == 'l') fmt++; }
        switch (*fmt) {
        case 'd': {
            int64_t v = is_long ? va_arg(ap, int64_t) : (int64_t)va_arg(ap, int32_t);
            if (v < 0) { serial_putc('-'); v = -v; }
            print_u((uint64_t)v, 10, width, pad);
            break;
        }
        case 'u':
            print_u(is_long ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, uint32_t),
                    10, width, pad);
            break;
        case 'x':
            print_u(is_long ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, uint32_t),
                    16, width, pad);
            break;
        case 'p':
            sputs("0x");
            print_u((uint64_t)(uintptr_t)va_arg(ap, void *), 16, 16, '0');
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            sputs(s ? s : "(null)");
            break;
        }
        case 'c':
            serial_putc((char)va_arg(ap, int));
            break;
        case '%': serial_putc('%'); break;
        case '\0': return;
        default: serial_putc('%'); serial_putc(*fmt); break;
        }
    }
}

void serial_printf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vprintf_shim(fmt, ap);
    va_end(ap);
}

void kprintf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vprintf_shim(fmt, ap);
    va_end(ap);
}

void panic(const char *msg) {
    __asm__ volatile ("cli");
    serial_printf("\n*** KERNEL PANIC: %s ***\n", msg);
    for (;;) __asm__ volatile ("hlt");
}

void boot_log_enable(void) { }
void boot_log_flush(const char *path) { (void)path; }

/* --- serial.h stubs so anything that #include's it links ------- */
void serial_putchar(char c) { if (c == '\n') serial_putc('\r'); serial_putc(c); }
