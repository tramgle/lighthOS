#ifndef ISR_H
#define ISR_H

#include "include/types.h"

typedef struct {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;  /* pusha */
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags, useresp, ss;              /* pushed by CPU */
} registers_t;

typedef registers_t *(*isr_handler_t)(registers_t *regs);

void isr_register_handler(uint8_t n, isr_handler_t handler);
registers_t *isr_handler(registers_t *regs);

#endif
