#include "lib/kprintf.h"
#include "drivers/vga.h"
#include "drivers/serial.h"
#include "lib/string.h"

static void kputchar(char c, bool to_vga, bool to_serial) {
    if (to_vga)    vga_putchar(c);
    if (to_serial) {
        if (c == '\n') serial_putchar('\r');
        serial_putchar(c);
    }
}

static void kputs_out(const char *s, bool to_vga, bool to_serial) {
    while (*s) kputchar(*s++, to_vga, to_serial);
}

static void print_uint(uint32_t val, int base, bool to_vga, bool to_serial) {
    char buf[32];
    int i = 0;
    static const char digits[] = "0123456789abcdef";

    if (val == 0) {
        kputchar('0', to_vga, to_serial);
        return;
    }
    while (val > 0) {
        buf[i++] = digits[val % base];
        val /= base;
    }
    while (--i >= 0) {
        kputchar(buf[i], to_vga, to_serial);
    }
}

static void print_int(int32_t val, bool to_vga, bool to_serial) {
    if (val < 0) {
        kputchar('-', to_vga, to_serial);
        print_uint((uint32_t)(-val), 10, to_vga, to_serial);
    } else {
        print_uint((uint32_t)val, 10, to_vga, to_serial);
    }
}

static void kvprintf(const char *fmt, va_list args, bool to_vga, bool to_serial) {
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            kputchar(*fmt, to_vga, to_serial);
            continue;
        }
        fmt++;
        switch (*fmt) {
        case 'd':
            print_int(va_arg(args, int32_t), to_vga, to_serial);
            break;
        case 'u':
            print_uint(va_arg(args, uint32_t), 10, to_vga, to_serial);
            break;
        case 'x':
            print_uint(va_arg(args, uint32_t), 16, to_vga, to_serial);
            break;
        case 'p':
            kputs_out("0x", to_vga, to_serial);
            print_uint(va_arg(args, uint32_t), 16, to_vga, to_serial);
            break;
        case 's': {
            const char *s = va_arg(args, const char *);
            kputs_out(s ? s : "(null)", to_vga, to_serial);
            break;
        }
        case 'c':
            kputchar((char)va_arg(args, int), to_vga, to_serial);
            break;
        case '%':
            kputchar('%', to_vga, to_serial);
            break;
        case '\0':
            return;
        default:
            kputchar('%', to_vga, to_serial);
            kputchar(*fmt, to_vga, to_serial);
            break;
        }
    }
}

void kprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    kvprintf(fmt, args, true, true);
    va_end(args);
}

void serial_printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    kvprintf(fmt, args, false, true);
    va_end(args);
}
