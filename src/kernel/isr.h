#ifndef ISR_H
#define ISR_H

#include "include/types.h"

/* x86_64 interrupt-entry register layout.
 *
 * Stack layout on entry to isr_handler (as built by isr_stub.s):
 *   rax, rbx, rcx, rdx, rsi, rdi, rbp,
 *   r8, r9, r10, r11, r12, r13, r14, r15,
 *   int_no, err_code,                 ; pushed by stub (+ CPU for 8,10-14,17)
 *   rip, cs, rflags, rsp, ss          ; pushed by CPU on interrupt
 *
 * All fields are uint64_t so the stub can use plain push/pop.
 * cs and ss are 16-bit on the wire but stored sign-extended here.
 */
typedef struct {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t int_no, err_code;
    uint64_t rip, cs, rflags, rsp, ss;
} registers_t;

typedef registers_t *(*isr_handler_t)(registers_t *regs);

void isr_register_handler(uint8_t n, isr_handler_t handler);
registers_t *isr_handler(registers_t *regs);

#endif
