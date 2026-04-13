#include "kernel/pic.h"
#include "include/io.h"

void pic_init(void) {
    /* ICW1: initialize + expect ICW4 */
    outb(PIC1_CMD, 0x11);
    io_wait();
    outb(PIC2_CMD, 0x11);
    io_wait();

    /* ICW2: vector offsets */
    outb(PIC1_DATA, IRQ_OFFSET_MASTER);  /* master: IRQ 0-7 -> INT 32-39 */
    io_wait();
    outb(PIC2_DATA, IRQ_OFFSET_SLAVE);   /* slave:  IRQ 8-15 -> INT 40-47 */
    io_wait();

    /* ICW3: cascading */
    outb(PIC1_DATA, 0x04);  /* slave on IRQ2 */
    io_wait();
    outb(PIC2_DATA, 0x02);  /* cascade identity */
    io_wait();

    /* ICW4: 8086 mode */
    outb(PIC1_DATA, 0x01);
    io_wait();
    outb(PIC2_DATA, 0x01);
    io_wait();

    /* Mask all IRQs except cascade (IRQ2) */
    outb(PIC1_DATA, 0xFB);  /* 1111 1011 — only IRQ2 unmasked */
    outb(PIC2_DATA, 0xFF);  /* all masked */
}

void pic_send_eoi(uint8_t irq) {
    if (irq >= 8) {
        outb(PIC2_CMD, 0x20);
    }
    outb(PIC1_CMD, 0x20);
}

void pic_set_mask(uint8_t irq) {
    uint16_t port;
    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }
    outb(port, inb(port) | (1 << irq));
}

void pic_clear_mask(uint8_t irq) {
    uint16_t port;
    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }
    outb(port, inb(port) & ~(1 << irq));
}
