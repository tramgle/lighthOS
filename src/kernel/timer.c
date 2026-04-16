#include "kernel/timer.h"
#include "kernel/isr.h"
#include "kernel/task.h"
#include "kernel/process.h"
#include "kernel/pic.h"
#include "include/io.h"
#include "lib/kprintf.h"

#define PIT_CHANNEL0 0x40
#define PIT_CMD      0x43
#define PIT_FREQ     1193182

static volatile uint32_t tick_count = 0;

static registers_t *timer_callback(registers_t *regs) {
    tick_count++;
    /* Drive SYS_ALARM timeouts. IRQ context: interrupts are masked,
       and the walk is bounded (PROCESS_MAX=16) so it's cheap to do
       unconditionally. schedule() runs after so a newly queued
       SIG_ALRM can be delivered on return if we end up re-entering
       the target task. */
    process_tick_alarms();
    return schedule(regs);
}

void timer_init(uint32_t frequency_hz) {
    uint32_t divisor = PIT_FREQ / frequency_hz;

    outb(PIT_CMD, 0x36);
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));

    isr_register_handler(32, timer_callback);
    pic_clear_mask(0);
}

uint32_t timer_get_ticks(void) {
    return tick_count;
}
