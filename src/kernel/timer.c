#include "kernel/timer.h"
#include "kernel/isr.h"
#include "kernel/pic.h"
#include "include/io.h"

#define PIT_CHANNEL0 0x40
#define PIT_CMD      0x43
#define PIT_FREQ     1193182

static volatile uint32_t tick_count = 0;

static void timer_callback(registers_t *regs) {
    (void)regs;
    tick_count++;
}

void timer_init(uint32_t frequency_hz) {
    uint32_t divisor = PIT_FREQ / frequency_hz;

    outb(PIT_CMD, 0x36);  /* channel 0, lo/hi byte, mode 3 (square wave) */
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));

    isr_register_handler(32, timer_callback);
    pic_clear_mask(0);  /* unmask IRQ0 */
}

uint32_t timer_get_ticks(void) {
    return tick_count;
}
