/* ld-lighthos.so.1 — static ET_EXEC interpreter at a fixed VA
 * above the main exec's layout but below the shared-lib region
 * at 0x30000000. Kept simple: one contiguous R/W/X segment. */

ENTRY(_start)
OUTPUT_FORMAT(elf64-x86-64)

SECTIONS
{
    . = 0x40000000;
    .text   : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data   : { *(.data*) }
    .bss    : { *(.bss*) *(COMMON) }
    _end = .;
}
