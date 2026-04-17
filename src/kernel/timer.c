#include "kernel/timer.h"
#include "kernel/isr.h"
#include "kernel/task.h"
#include "kernel/pic.h"
#include "kernel/process.h"
#include "include/io.h"
#include "lib/kprintf.h"

#define PIT_CHANNEL0 0x40
#define PIT_CMD      0x43
#define PIT_FREQ     1193182

static volatile uint64_t tick_count = 0;

extern void process_tick_alarms(void);

static registers_t *timer_callback(registers_t *regs) {
    tick_count++;
    /* Attribute this tick to the currently-running process. We
       bucket by the interrupted frame's privilege (CS&3==3 =
       user). The idle task has no backing process, so
       process_current() returns NULL and the tick is unattributed. */
    process_t *p = process_current();
    if (p) {
        if ((regs->cs & 3) == 3) p->utime_ticks++;
        else                     p->stime_ticks++;
    }
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

uint64_t timer_get_ticks(void) { return tick_count; }
