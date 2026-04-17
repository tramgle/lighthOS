/* PS/2 mouse driver.
 *
 * Controller init sequence (Intel 8042 / KBC):
 *   1. enable aux port (cmd 0xA8)
 *   2. read config byte (cmd 0x20), set bit 1 (enable IRQ12),
 *      clear bit 5 (enable aux clock), write back (cmd 0x60)
 *   3. aux set-defaults (0xF6) — resets resolution/scaling
 *   4. aux enable-reporting (0xF4) — starts sending packets
 *
 * Each command to the aux device has to be prefixed with 0xD4 on
 * the command port so the next write to the data port lands on
 * the mouse rather than the keyboard.
 *
 * IRQ12 (INT 44) fires one byte at a time. Assemble the 3-byte
 * packet in a tiny state machine, resync on any byte-0 that
 * doesn't have bit 3 set (the "always 1" flag is the canonical
 * resync hint). */

#include "drivers/mouse.h"
#include "kernel/isr.h"
#include "kernel/pic.h"
#include "include/io.h"
#include "lib/kprintf.h"

#define KBD_DATA 0x60
#define KBD_CMD  0x64

/* Controller status bits at 0x64. */
#define KBC_OUT_FULL 0x01
#define KBC_IN_FULL  0x02
/* Bit 5 of the status byte is AUX_OUT_FULL — tells us the pending
 * byte in 0x60 came from the aux device, not the keyboard. We don't
 * rely on it for IRQ12 (the IRQ itself is authoritative), but the
 * init sequence uses it to drain aux responses without racing with a
 * pending keyboard scancode. */
#define KBC_AUX_OUT  0x20

static int32_t  mouse_x, mouse_y;
static uint32_t mouse_buttons;
static uint32_t extent_w = 320, extent_h = 200;

/* Packet assembly state. packet_len counts bytes accepted so far;
 * packet[0..packet_len-1] hold them. On the third byte we commit
 * to mouse_x/mouse_y/mouse_buttons and reset. */
static uint8_t  packet[3];
static uint32_t packet_len;

static void wait_write(void) {
    /* Spin until the controller's input buffer is empty — writing
       to 0x60/0x64 while a previous command is pending is ignored. */
    for (int i = 0; i < 100000; i++) {
        if ((inb(KBD_CMD) & KBC_IN_FULL) == 0) return;
    }
}

static void wait_read(void) {
    /* Spin until the controller has a byte to hand back. */
    for (int i = 0; i < 100000; i++) {
        if (inb(KBD_CMD) & KBC_OUT_FULL) return;
    }
}

static void mouse_send(uint8_t byte) {
    wait_write();
    outb(KBD_CMD, 0xD4);   /* next data-port write targets aux */
    wait_write();
    outb(KBD_DATA, byte);
}

static uint8_t mouse_read(void) {
    wait_read();
    return inb(KBD_DATA);
}

static void clamp_xy(void) {
    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_x >= (int32_t)extent_w) mouse_x = (int32_t)extent_w - 1;
    if (mouse_y >= (int32_t)extent_h) mouse_y = (int32_t)extent_h - 1;
}

static registers_t *mouse_irq(registers_t *regs) {
    uint8_t byte = inb(KBD_DATA);

    /* Resync: byte 0 must have bit 3 set (the "always 1" flag) and
       must not have an overflow bit set. If either fails, drop
       whatever we had and try again on the next IRQ. */
    if (packet_len == 0 && ((byte & 0x08) == 0 || (byte & 0xC0))) {
        return regs;
    }

    packet[packet_len++] = byte;
    if (packet_len < 3) return regs;
    packet_len = 0;

    uint8_t b0 = packet[0];
    /* Integrate 9-bit deltas. Sign bits live in byte 0. */
    int16_t dx = (int16_t)packet[1] - ((b0 & 0x10) ? 256 : 0);
    int16_t dy = (int16_t)packet[2] - ((b0 & 0x20) ? 256 : 0);

    mouse_x += dx;
    mouse_y -= dy;           /* PS/2 "up is positive" -> screen Y flips */
    clamp_xy();

    mouse_buttons = (uint32_t)(b0 & 0x07);
    return regs;
}

void mouse_set_extent(uint32_t width, uint32_t height) {
    if (width)  extent_w = width;
    if (height) extent_h = height;
    clamp_xy();
}

void mouse_get_state(int32_t *x_out, int32_t *y_out, uint32_t *buttons_out) {
    if (x_out)       *x_out = mouse_x;
    if (y_out)       *y_out = mouse_y;
    if (buttons_out) *buttons_out = mouse_buttons;
}

void mouse_init(void) {
    mouse_x = (int32_t)(extent_w / 2);
    mouse_y = (int32_t)(extent_h / 2);
    mouse_buttons = 0;
    packet_len = 0;

    /* Enable aux port. */
    wait_write();
    outb(KBD_CMD, 0xA8);

    /* Read + rewrite controller config: enable IRQ12, enable aux
       clock. */
    wait_write();
    outb(KBD_CMD, 0x20);
    uint8_t status = mouse_read();
    status |=  0x02;       /* enable IRQ12 */
    status &= ~0x20;       /* clear disable-aux-clock bit */
    wait_write();
    outb(KBD_CMD, 0x60);
    wait_write();
    outb(KBD_DATA, status);

    /* Aux set-defaults. Swallow the ACK so it doesn't masquerade as
       a bogus first packet byte to the IRQ handler. */
    mouse_send(0xF6);
    (void)mouse_read();

    /* Aux enable-reporting. From here on IRQ12 will fire. */
    mouse_send(0xF4);
    (void)mouse_read();

    isr_register_handler(44, mouse_irq);   /* IRQ12 = INT 44 */
    pic_clear_mask(12);
    /* IRQ2 (slave cascade) was already unmasked by pic_init. */

    serial_printf("[mouse] PS/2 mouse enabled\n");
}
