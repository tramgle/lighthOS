; jump_to_usermode(uint64_t entry, uint64_t user_stack)
; Drops to ring 3 via iretq. SysV AMD64: entry in RDI, stack in RSI.
;
; CPU pops SS, RSP, RFLAGS, CS, RIP in that order from the kernel
; stack on iretq. We push them here.

bits 64
global jump_to_usermode

jump_to_usermode:
    ; data-seg selectors (user data = 0x20 | RPL 3 = 0x23)
    mov ax, 0x23
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; Build iretq frame (top of stack after pushes is RIP).
    push qword 0x23                 ; SS  = user data | RPL 3
    push rsi                        ; RSP = user stack
    pushfq
    pop rax
    or  rax, 0x200                  ; IF=1 so IRQs are live in user
    push rax                        ; RFLAGS
    push qword 0x2B                 ; CS  = user code | RPL 3  (0x28|3)
    push rdi                        ; RIP = entry
    iretq
