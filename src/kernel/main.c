/*
 * L1 minimal kernel_main — proves the 32→64 trampoline in
 * src/boot/boot.s lands cleanly in a higher-half C entry point and
 * can talk to COM1.
 *
 * Deliberately self-contained (direct port I/O, no drivers/ pulled
 * in) so this milestone doesn't depend on any subsystem that still
 * has 32-bit inline asm or register-width bugs. As each subsystem
 * is ported (L2 pmm/vmm, L3 interrupts, L4 processes), this file
 * will grow back toward main.c.pre_l5.
 */

#include <stdint.h>

#define COM1 0x3F8

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static void serial_init_l1(void) {
    outb(COM1 + 1, 0x00);  /* disable interrupts */
    outb(COM1 + 3, 0x80);  /* DLAB on */
    outb(COM1 + 0, 0x03);  /* divisor low (38400 baud) */
    outb(COM1 + 1, 0x00);  /* divisor high */
    outb(COM1 + 3, 0x03);  /* 8N1, DLAB off */
    outb(COM1 + 2, 0xC7);  /* enable + clear FIFO */
    outb(COM1 + 4, 0x0B);  /* RTS/DTR set */
}

static void serial_putc_l1(char c) {
    while ((inb(COM1 + 5) & 0x20) == 0) { }
    outb(COM1, (uint8_t)c);
}

static void serial_puts_l1(const char *s) {
    for (; *s; s++) {
        if (*s == '\n') serial_putc_l1('\r');
        serial_putc_l1(*s);
    }
}

static void serial_puthex64(uint64_t v) {
    static const char hex[] = "0123456789abcdef";
    char buf[19];
    buf[0] = '0'; buf[1] = 'x';
    for (int i = 0; i < 16; i++) {
        buf[2 + i] = hex[(v >> (60 - 4*i)) & 0xF];
    }
    buf[18] = '\0';
    serial_puts_l1(buf);
}

void kernel_main(uint32_t magic, void *mbi) {
    serial_init_l1();
    serial_puts_l1("\n");
    serial_puts_l1("================================\n");
    serial_puts_l1("LighthOS L1: long mode online\n");
    serial_puts_l1("================================\n");
    serial_puts_l1("  multiboot magic : ");
    serial_puthex64((uint64_t)magic);
    serial_puts_l1("\n  mbi pointer    : ");
    serial_puthex64((uint64_t)(uintptr_t)mbi);
    serial_puts_l1("\n  kernel_main RIP: ");
    uint64_t rip;
    __asm__ volatile ("leaq (%%rip), %0" : "=r"(rip));
    serial_puthex64(rip);
    serial_puts_l1("\n  RSP            : ");
    uint64_t rsp;
    __asm__ volatile ("mov %%rsp, %0" : "=r"(rsp));
    serial_puthex64(rsp);
    serial_puts_l1("\n\nhalting.\n");

    for (;;) __asm__ volatile ("cli; hlt");
}
