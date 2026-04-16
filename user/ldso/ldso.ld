/* ld-vibeos.so.1 linker script.
   Places the interpreter at a high fixed address (0x40000000) far
   from the main executable's text (0x08048000..) and heap
   (0x08100000..). Statically linked, ET_EXEC — not PIC. Runs in the
   same address space as the main program it serves; when it jumps
   into main's entry, control flows "forward" into user code and the
   interp's .text stays resident (main may later make syscalls that
   don't touch it). */
ENTRY(_start)
SECTIONS {
    . = 0x40000000;
    .text   : { *(.text) }
    .rodata : { *(.rodata) }
    .data   : { *(.data) }
    .bss    : { *(.bss) *(COMMON) }
    _end = .;
}
