; Multiboot constants
MBOOT_MAGIC     equ 0x1BADB002
MBOOT_FLAGS     equ 0x00000003      ; align modules + provide memory map
MBOOT_CHECKSUM  equ -(MBOOT_MAGIC + MBOOT_FLAGS)

section .multiboot
align 4
    dd MBOOT_MAGIC
    dd MBOOT_FLAGS
    dd MBOOT_CHECKSUM

section .bss
align 16
stack_bottom:
    resb 16384                      ; 16 KB kernel stack
stack_top:

section .text
global _start
extern kernel_main

_start:
    mov esp, stack_top
    push ebx                        ; multiboot info struct pointer
    push eax                        ; multiboot magic number
    call kernel_main

    ; Should never return, but just in case
    cli
.hang:
    hlt
    jmp .hang
