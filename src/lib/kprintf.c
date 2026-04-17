/* Kernel printf. Writes to both the VGA text console and COM1.
 *
 * The boot log captures everything kputchar writes into a fixed
 * BSS buffer so the early-boot chatter (before any fs is up) is
 * preserved. Once a writable fs is mounted, call boot_log_flush
 * to dump the buffer to a file.
 *
 * Format support: %d %u %x %p %s %c %% plus width/pad and length
 * prefix %l (64-bit). %lu / %lx / %ld are essential on x86_64
 * where most kernel pointers and sizes are 64-bit.
 */

#include "lib/kprintf.h"
#include "drivers/vga.h"
#include "drivers/serial.h"
#include "lib/string.h"

extern int vfs_create(const char *path, uint32_t type);
extern int vfs_stat(const char *path, void *st);
extern int vfs_write(const char *path, const void *buf, uint32_t n, uint32_t off);

#define BOOT_LOG_MAX 16384
static char     boot_log_buf[BOOT_LOG_MAX];
static uint32_t boot_log_head;       /* next write slot */
static bool     boot_log_wrapped;    /* ring has cycled at least once */
static bool     boot_log_on;
static volatile int boot_log_flushing;  /* re-entrance guard for panic path */

void boot_log_enable(void) { boot_log_on = true; }

/* Flush the boot-log ring to `path` as text. When the ring has
 * wrapped, the write happens in two parts — [head..MAX) then
 * [0..head) — so the file ends up in time order even for a
 * long-lived kernel whose recent messages evicted the earliest
 * boot chatter.
 *
 * Silently no-ops on re-entrance: a vfs_write that faults and
 * panics would otherwise recurse into the flush path forever. */
void boot_log_flush(const char *path) {
    if (!path || boot_log_flushing) return;
    boot_log_flushing = 1;

    if (boot_log_wrapped) {
        vfs_create(path, 1 /* VFS_FILE */);
        uint32_t tail = BOOT_LOG_MAX - boot_log_head;
        vfs_write(path, boot_log_buf + boot_log_head, tail, 0);
        vfs_write(path, boot_log_buf, boot_log_head, tail);
    } else if (boot_log_head > 0) {
        vfs_create(path, 1 /* VFS_FILE */);
        vfs_write(path, boot_log_buf, boot_log_head, 0);
    }

    boot_log_flushing = 0;
}

static void kputchar(char c, bool to_vga, bool to_serial) {
    if (to_vga)    vga_putchar(c);
    if (to_serial) {
        if (c == '\n') serial_putchar('\r');
        serial_putchar(c);
    }
    if (boot_log_on) {
        boot_log_buf[boot_log_head++] = c;
        if (boot_log_head >= BOOT_LOG_MAX) {
            boot_log_head = 0;
            boot_log_wrapped = true;
        }
    }
}

static void kputs_out(const char *s, bool to_vga, bool to_serial) {
    while (*s) kputchar(*s++, to_vga, to_serial);
}

static void print_u(uint64_t val, int base, int width, char pad,
                    bool to_vga, bool to_serial) {
    static const char dig[] = "0123456789abcdef";
    char buf[32];
    int i = 0;
    if (val == 0) buf[i++] = '0';
    while (val > 0) { buf[i++] = dig[val % base]; val /= (uint64_t)base; }
    while (i < width) buf[i++] = pad;
    while (--i >= 0) kputchar(buf[i], to_vga, to_serial);
}

static void kvprintf(const char *fmt, va_list ap, bool to_vga, bool to_serial) {
    for (; *fmt; fmt++) {
        if (*fmt != '%') { kputchar(*fmt, to_vga, to_serial); continue; }
        fmt++;
        int width = 0; char pad = ' ';
        if (*fmt == '0') { pad = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') { width = width*10 + (*fmt - '0'); fmt++; }
        int is_long = 0;
        if (*fmt == 'l') { is_long = 1; fmt++; if (*fmt == 'l') fmt++; }
        switch (*fmt) {
        case 'd': {
            int64_t v = is_long ? va_arg(ap, int64_t) : (int64_t)va_arg(ap, int32_t);
            if (v < 0) { kputchar('-', to_vga, to_serial); v = -v; }
            print_u((uint64_t)v, 10, width, pad, to_vga, to_serial);
            break;
        }
        case 'u':
            print_u(is_long ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, uint32_t),
                    10, width, pad, to_vga, to_serial);
            break;
        case 'x':
            print_u(is_long ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, uint32_t),
                    16, width, pad, to_vga, to_serial);
            break;
        case 'p':
            kputs_out("0x", to_vga, to_serial);
            print_u((uint64_t)(uintptr_t)va_arg(ap, void *), 16, 16, '0',
                    to_vga, to_serial);
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            kputs_out(s ? s : "(null)", to_vga, to_serial);
            break;
        }
        case 'c': kputchar((char)va_arg(ap, int), to_vga, to_serial); break;
        case '%': kputchar('%', to_vga, to_serial); break;
        case '\0': return;
        default:  kputchar('%', to_vga, to_serial); kputchar(*fmt, to_vga, to_serial); break;
        }
    }
}

void kprintf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); kvprintf(fmt, ap, true, true); va_end(ap);
}

void serial_printf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); kvprintf(fmt, ap, false, true); va_end(ap);
}
