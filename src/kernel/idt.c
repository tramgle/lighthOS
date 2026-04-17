/* x86_64 IDT: 256 × 16-byte gate descriptors.
 *
 *   bits 0..15   offset 15..0
 *   bits 16..31  selector
 *   bits 32..34  IST index (0 = no IST)
 *   bits 40..47  type + DPL + P (0xEE for INT gate DPL=3,
 *                                0x8E for INT gate DPL=0)
 *   bits 48..63  offset 31..16
 *   bits 64..95  offset 63..32
 *   bits 96..127 reserved (zero)
 */

#include "kernel/idt.h"
#include "kernel/gdt.h"
#include "lib/string.h"

typedef struct {
    uint16_t off_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t off_mid;
    uint32_t off_high;
    uint32_t zero;
} __attribute__((packed)) idt_entry_t;

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static idt_entry_t idt[256];
static struct idt_ptr idtp;

extern void idt_flush(struct idt_ptr *ptr);

/* Stub table declared by isr_stub.s: isr_entry[0..255] each jumps
   into the common dispatch after pushing (err, num). A single
   array simplifies the init loop compared to 256 named externs. */
extern void *isr_entry[256];

void idt_set_gate(uint8_t num, uint64_t handler, uint16_t selector,
                  uint8_t ist, uint8_t type_dpl) {
    idt[num].off_low   = handler & 0xFFFF;
    idt[num].off_mid   = (handler >> 16) & 0xFFFF;
    idt[num].off_high  = (handler >> 32) & 0xFFFFFFFFu;
    idt[num].selector  = selector;
    idt[num].ist       = ist & 0x7;
    idt[num].type_attr = type_dpl;
    idt[num].zero      = 0;
}

void idt_init(void) {
    memset(idt, 0, sizeof(idt));

    for (int i = 0; i < 256; i++) {
        uint8_t ist = 0;
        uint8_t attr = 0x8E;                /* present, DPL=0, INT gate */
        /* Put #DF (8), #NMI (2), #MC (18), #PF (14) on IST1 so a
           bad kernel stack can't trigger a further fault. */
        if (i == 2 || i == 8 || i == 14 || i == 18) ist = 1;
        /* INT 0x80 = syscall: must be reachable from ring 3. */
        if (i == 0x80) attr = 0xEE;
        idt_set_gate((uint8_t)i, (uint64_t)isr_entry[i],
                     GDT_KERNEL_CODE, ist, attr);
    }

    idtp.limit = sizeof(idt) - 1;
    idtp.base  = (uint64_t)(uintptr_t)&idt[0];
    idt_flush(&idtp);
}
