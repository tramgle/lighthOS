/* x86_64 user linker script. Static ET_EXEC at 4 MiB VA.
 * Replaces the i386 0x08048000 base now that user VA lives in
 * the canonical low half above USER_VA_MIN (kernel/elf.c). */

ENTRY(_start)
OUTPUT_FORMAT(elf64-x86-64)

SECTIONS
{
    . = 0x400000;
    .text   : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data   : { *(.data*) }
    .bss    : { *(.bss*) *(COMMON) }
    _end = .;
}
