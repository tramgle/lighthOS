#include "kernel/gdt.h"

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

static struct gdt_entry gdt[5];
static struct gdt_ptr   gdtp;

extern void gdt_flush(struct gdt_ptr *ptr);

static void gdt_set_entry(int idx, uint32_t base, uint32_t limit,
                           uint8_t access, uint8_t gran) {
    gdt[idx].base_low    = base & 0xFFFF;
    gdt[idx].base_mid    = (base >> 16) & 0xFF;
    gdt[idx].base_high   = (base >> 24) & 0xFF;
    gdt[idx].limit_low   = limit & 0xFFFF;
    gdt[idx].granularity = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt[idx].access      = access;
}

void gdt_init(void) {
    gdtp.limit = sizeof(gdt) - 1;
    gdtp.base  = (uint32_t)&gdt;

    gdt_set_entry(0, 0, 0,       0x00, 0x00);  /* Null */
    gdt_set_entry(1, 0, 0xFFFFF, 0x9A, 0xCF);  /* Kernel code */
    gdt_set_entry(2, 0, 0xFFFFF, 0x92, 0xCF);  /* Kernel data */
    gdt_set_entry(3, 0, 0xFFFFF, 0xFA, 0xCF);  /* User code */
    gdt_set_entry(4, 0, 0xFFFFF, 0xF2, 0xCF);  /* User data */

    gdt_flush(&gdtp);
}
