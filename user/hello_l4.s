; L4 pilot user binary.
;
; A minimal static ELF64 that proves the full "load + iretq + INT
; 0x80 + sysret" pipe works end-to-end. It calls INT 0x80 with a
; sentinel vector + argument register pattern the kernel recognises
; as the L4 self-test opcode, then halts via a second INT.
;
; Kernel contract (L4-only):
;   RAX = 0x4C54     — "L4-test write" opcode
;   RDI = char *     — string pointer
;   RSI = size_t     — string length
;   RAX = 0x4C58     — "L4-test exit" opcode
;   RDI = exit code
;
; No libc, no crt0. Direct ABI. Lives at VA 0x400000 (user.ld).

bits 64
section .text
global _start

_start:
    lea rdi, [rel msg]
    mov rsi, msg_end - msg
    mov rax, 0x4C54
    int 0x80

    mov rdi, 0
    mov rax, 0x4C58
    int 0x80

.hang:
    jmp .hang

section .rodata
msg:
    db  "Hello from ring 3!", 10
msg_end:
