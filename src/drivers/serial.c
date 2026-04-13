#include "drivers/serial.h"
#include "drivers/keyboard.h"  /* KEY_UP, KEY_DOWN, etc. */
#include "kernel/isr.h"
#include "kernel/pic.h"
#include "include/io.h"

#define SERIAL_BUF_SIZE 256

static char serial_buffer[SERIAL_BUF_SIZE];
static volatile uint32_t serial_read_idx;
static volatile uint32_t serial_write_idx;

static int serial_is_transmit_empty(void) {
    return inb(SERIAL_COM1 + 5) & 0x20;
}

static void serial_enqueue(char c) {
    uint32_t next = (serial_write_idx + 1) % SERIAL_BUF_SIZE;
    if (next != serial_read_idx) {
        serial_buffer[serial_write_idx] = c;
        serial_write_idx = next;
    }
}

static uint8_t esc_state;  /* 0=normal, 1=got ESC, 2=got ESC[ */

static void serial_callback(registers_t *regs) {
    (void)regs;
    while (inb(SERIAL_COM1 + 5) & 0x01) {  /* data ready */
        char c = inb(SERIAL_COM1);

        /* ESC sequence state machine: ESC [ A/B/C/D -> arrow keys */
        if (esc_state == 0 && c == 0x1B) {
            esc_state = 1;
            continue;
        }
        if (esc_state == 1) {
            if (c == '[') { esc_state = 2; continue; }
            esc_state = 0;  /* not a CSI sequence, drop the ESC */
            continue;
        }
        if (esc_state == 2) {
            esc_state = 0;
            switch (c) {
            case 'A': serial_enqueue(KEY_UP);    continue;
            case 'B': serial_enqueue(KEY_DOWN);  continue;
            case 'C': serial_enqueue(KEY_RIGHT); continue;
            case 'D': serial_enqueue(KEY_LEFT);  continue;
            }
            continue;  /* unknown CSI sequence, drop */
        }

        if (c == '\r') c = '\n';
        if (c == 0x7F) c = '\b';
        serial_enqueue(c);
    }
}

void serial_init(void) {
    serial_read_idx = 0;
    serial_write_idx = 0;

    outb(SERIAL_COM1 + 1, 0x00);  /* Disable interrupts during setup */
    outb(SERIAL_COM1 + 3, 0x80);  /* Enable DLAB */
    outb(SERIAL_COM1 + 0, 0x01);  /* Divisor 1 = 115200 baud */
    outb(SERIAL_COM1 + 1, 0x00);  /* High byte of divisor */
    outb(SERIAL_COM1 + 3, 0x03);  /* 8 bits, no parity, 1 stop */
    outb(SERIAL_COM1 + 2, 0xC7);  /* Enable FIFO, clear, 14-byte */
    outb(SERIAL_COM1 + 4, 0x0B);  /* RTS/DSR set */
}

void serial_init_irq(void) {
    outb(SERIAL_COM1 + 1, 0x01);  /* Enable receive data available interrupt */
    isr_register_handler(36, serial_callback);  /* IRQ4 = INT 36 */
    pic_clear_mask(4);  /* unmask IRQ4 */
}

void serial_putchar(char c) {
    while (!serial_is_transmit_empty());
    outb(SERIAL_COM1, c);
}

void serial_puts(const char *s) {
    while (*s) {
        if (*s == '\n') serial_putchar('\r');
        serial_putchar(*s++);
    }
}

bool serial_has_data(void) {
    return serial_read_idx != serial_write_idx;
}

char serial_getchar(void) {
    while (!serial_has_data()) {
        __asm__ volatile ("hlt");
    }
    char c = serial_buffer[serial_read_idx];
    serial_read_idx = (serial_read_idx + 1) % SERIAL_BUF_SIZE;
    return c;
}
