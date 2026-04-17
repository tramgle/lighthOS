bits 64
global tss_flush

tss_flush:
    mov ax, 0x30            ; TSS selector
    ltr ax
    ret
