; jump_to_usermode(uint32_t entry, uint32_t user_stack)
; Drops to ring 3 by doing an iret with user CS/SS selectors.
global jump_to_usermode

jump_to_usermode:
    mov ebx, [esp+4]    ; entry point
    mov ecx, [esp+8]    ; user stack pointer

    ; Set data segments to user data selector (0x20 | RPL 3 = 0x23)
    mov ax, 0x23
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; Build iret frame for ring 0 -> ring 3 transition
    ; CPU expects: SS, ESP, EFLAGS, CS, EIP (pushed in this order)
    push dword 0x23     ; ss = user data
    push ecx            ; esp = user stack
    pushfd
    pop eax
    or eax, 0x200       ; ensure IF (interrupts enabled)
    push eax            ; eflags
    push dword 0x1B     ; cs = user code (0x18 | RPL 3)
    push ebx            ; eip = entry point
    iret
