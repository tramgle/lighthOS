/*
 * L3 kernel_main — wires GDT + TSS + IDT + PIC and round-trips
 * an interrupt (both a software INT 0x80 and the timer IRQ0).
 *
 * Still a port-era kernel: no processes, no syscall dispatcher,
 * no userland. The self-test installs a one-shot handler, raises
 * INT 0x80, confirms the handler ran, then enables IRQs and waits
 * for the timer to prove IRQ path works too.
 */

#include "include/types.h"
#include "include/multiboot.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "kernel/gdt.h"
#include "kernel/tss.h"
#include "kernel/idt.h"
#include "kernel/isr.h"
#include "kernel/pic.h"

/* Task-system stubs — vmm.c references these; L4 will implement. */
struct task;
struct task *task_current(void) { return (void *)0; }
uint64_t *task_current_pml4(void) { return (uint64_t *)0; }

extern void serial_init(void);
extern char stack_top;

static volatile int int80_fired;
static volatile int timer_tick_count;

static registers_t *int80_handler(registers_t *regs) {
    int80_fired = 1;
    serial_printf("[l3] INT 0x80 handler fired. rdi=0x%lx rsi=0x%lx\n",
                  regs->rdi, regs->rsi);
    return regs;
}

static registers_t *timer_handler(registers_t *regs) {
    timer_tick_count++;
    return regs;
}

static void install_timer(void) {
    /* PIT channel 0, rate generator, ~100 Hz */
    uint16_t div = 1193180 / 100;
    __asm__ volatile ("outb %0, $0x43" :: "a"((uint8_t)0x36));
    __asm__ volatile ("outb %0, $0x40" :: "a"((uint8_t)(div & 0xFF)));
    __asm__ volatile ("outb %0, $0x40" :: "a"((uint8_t)((div >> 8) & 0xFF)));
    pic_clear_mask(0);
    isr_register_handler(32, timer_handler);
}

static void l3_self_test(void) {
    serial_printf("\n[l3] self-test: installing INT 0x80 handler...\n");
    isr_register_handler(0x80, int80_handler);

    int80_fired = 0;
    __asm__ volatile (
        "mov $0xCAFEBABE, %%rdi\n\t"
        "mov $0x12345678, %%rsi\n\t"
        "int $0x80\n\t"
        ::: "rdi", "rsi", "memory"
    );
    serial_printf("[l3]   int80_fired=%d\n", int80_fired);

    serial_printf("[l3] enabling IRQs, waiting for timer...\n");
    install_timer();
    __asm__ volatile ("sti");
    int start = timer_tick_count;
    /* Busy-wait for ~50 ticks (half a second) to prove IRQs fire. */
    while (timer_tick_count - start < 50) {
        __asm__ volatile ("hlt");
    }
    serial_printf("[l3]   timer ticks: %d (delta=%d)\n",
                  timer_tick_count, timer_tick_count - start);
    __asm__ volatile ("cli");
}

void kernel_main(uint32_t magic, multiboot_info_t *mbi) {
    serial_init();

    serial_printf("\n================================\n");
    serial_printf("LighthOS L3: IDT + ISRs online\n");
    serial_printf("================================\n");
    serial_printf("  multiboot magic: 0x%x\n", magic);

    if (magic != MULTIBOOT_MAGIC) {
        serial_printf("  bad multiboot magic; halting.\n");
        for (;;) __asm__ volatile ("cli; hlt");
    }

    pmm_init(mbi);
    vmm_init();

    gdt_init();
    tss_init((uint64_t)(uintptr_t)&stack_top);
    pic_init();
    idt_init();

    l3_self_test();

    serial_printf("\n[l3] complete. halting.\n");
    for (;;) __asm__ volatile ("cli; hlt");
}
