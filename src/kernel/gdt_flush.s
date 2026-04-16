; Load the new GDT and reload every segment register.
;
; In long mode data segments (DS/ES/FS/GS/SS) are mostly ignored
; but the descriptor-cache reload still matters for correctness on
; some CPUs; we load them with the null or data selector. CS needs
; a far return (`retfq`) since you can't `mov %rcx, %cs`.

bits 64

global gdt_flush
gdt_flush:
    lgdt [rdi]

    mov ax, 0x10            ; kernel data
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Far return to reload CS with the new code descriptor.
    ; Push new CS:RIP and iretq... actually retfq is simpler.
    pop rax                 ; saved return RIP from `call gdt_flush`
    push qword 0x08         ; new CS
    push rax                ; return RIP
    retfq
