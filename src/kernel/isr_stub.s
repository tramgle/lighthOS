; x86_64 ISR stubs.
;
; For each vector 0..255 we emit a tiny entry that pushes a fake
; error code (where the CPU didn't) and the vector number, then
; jumps to the common dispatch. An address table `isr_entry[256]`
; lets idt.c wire them up in a loop.
;
; The stack when isr_common runs:
;   [rsp+0]    r15
;   [rsp+8]    r14
;    ...
;   [rsp+112]  rax
;   [rsp+120]  int_no
;   [rsp+128]  err_code    (real or fake)
;   [rsp+136]  rip         (CPU)
;   [rsp+144]  cs
;   [rsp+152]  rflags
;   [rsp+160]  rsp (user)
;   [rsp+168]  ss
;
; isr_handler receives RDI = pointer to registers_t (same shape),
; returns pointer to a registers_t to restore from (normally same,
; different for context switches).

bits 64

extern isr_handler

%macro ISR_NOERR 1
global isr%1
isr%1:
    push qword 0                ; dummy error code
    push qword %1               ; vector
    jmp isr_common
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
    ; CPU already pushed the real error code.
    push qword %1
    jmp isr_common
%endmacro

; CPU exceptions with error code on the stack: 8, 10-14, 17, 21, 29, 30
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31

; 32..255 — no CPU error code ever. Emit via %assign loop.
%assign i 32
%rep (256 - 32)
ISR_NOERR i
%assign i i+1
%endrep

; ---- isr_entry[256] pointer table ------------------------------
section .rodata
align 8
global isr_entry
isr_entry:
%assign i 0
%rep 256
    dq isr %+ i
%assign i i+1
%endrep

section .text
bits 64

; ---- Common dispatch -------------------------------------------
isr_common:
    ; Save general-purpose registers in registers_t order.
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rbp
    push rdi
    push rsi
    push rdx
    push rcx
    push rbx
    push rax

    ; After all the pushes, RSP points at .rax of registers_t.
    ; The layout so far, from low to high:
    ;   rax, rbx, rcx, rdx, rsi, rdi, rbp,
    ;   r8, r9, r10, r11, r12, r13, r14, r15,
    ;   int_no, err_code, rip, cs, rflags, rsp, ss
    ; — which matches registers_t in isr.h exactly.

    mov rdi, rsp                ; registers_t *regs
    call isr_handler
    mov rsp, rax                ; possibly switched stack

    pop rax
    pop rbx
    pop rcx
    pop rdx
    pop rsi
    pop rdi
    pop rbp
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15

    add rsp, 16                 ; pop int_no + err_code
    iretq
