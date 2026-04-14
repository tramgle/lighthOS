; ISR/IRQ assembly stubs
; Stack frame must match registers_t in isr.h:
;   gs, fs, es, ds, edi, esi, ebp, esp, ebx, edx, ecx, eax, int_no, err_code,
;   eip, cs, eflags, useresp, ss

extern isr_handler

; Macro for ISRs that do NOT push an error code (we push a dummy 0)
%macro ISR_NOERR 1
global isr%1
isr%1:
    push dword 0            ; dummy error code
    push dword %1           ; interrupt number
    jmp isr_common
%endmacro

; Macro for ISRs that DO push an error code
%macro ISR_ERR 1
global isr%1
isr%1:
    push dword %1           ; interrupt number (error code already on stack)
    jmp isr_common
%endmacro

; Macro for IRQ stubs
%macro IRQ 2
global irq%1
irq%1:
    push dword 0            ; dummy error code
    push dword %2           ; interrupt number (32 + irq)
    jmp isr_common
%endmacro

; CPU exceptions 0-31
ISR_NOERR 0     ; Division by zero
ISR_NOERR 1     ; Debug
ISR_NOERR 2     ; NMI
ISR_NOERR 3     ; Breakpoint
ISR_NOERR 4     ; Overflow
ISR_NOERR 5     ; Bound range exceeded
ISR_NOERR 6     ; Invalid opcode
ISR_NOERR 7     ; Device not available
ISR_ERR   8     ; Double fault
ISR_NOERR 9     ; Coprocessor segment overrun
ISR_ERR   10    ; Invalid TSS
ISR_ERR   11    ; Segment not present
ISR_ERR   12    ; Stack-segment fault
ISR_ERR   13    ; General protection fault
ISR_ERR   14    ; Page fault
ISR_NOERR 15    ; Reserved
ISR_NOERR 16    ; x87 FPU error
ISR_ERR   17    ; Alignment check
ISR_NOERR 18    ; Machine check
ISR_NOERR 19    ; SIMD floating-point
ISR_NOERR 20    ; Virtualization
ISR_ERR   21    ; Control protection
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29    ; VMM communication
ISR_ERR   30    ; Security exception
ISR_NOERR 31

; Hardware IRQs 0-15 -> INT 32-47
IRQ 0,  32
IRQ 1,  33
IRQ 2,  34
IRQ 3,  35
IRQ 4,  36
IRQ 5,  37
IRQ 6,  38
IRQ 7,  39
IRQ 8,  40
IRQ 9,  41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47

; Software interrupts (not hardware IRQs — no EOI needed)
ISR_NOERR 128   ; INT 0x80 = syscall
ISR_NOERR 130   ; INT 0x82 = yield

; Common handler: save all registers, call C handler, restore, iret
; isr_handler returns a registers_t* in eax (possibly a different stack
; for context switching). We use that as the new esp before restoring.
isr_common:
    pusha                   ; push eax, ecx, edx, ebx, esp, ebp, esi, edi
    push ds
    push es
    push fs
    push gs

    mov ax, 0x10            ; kernel data segment
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push esp                ; pointer to registers_t on stack
    call isr_handler
    mov esp, eax            ; use returned stack pointer (may be different task)

    pop gs
    pop fs
    pop es
    pop ds
    popa                    ; restore general-purpose registers
    add esp, 8              ; pop int_no and err_code
    iret
