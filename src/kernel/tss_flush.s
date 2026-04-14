global tss_flush

tss_flush:
    mov ax, 0x2B    ; TSS selector (0x28) | RPL 3
    ltr ax
    ret
