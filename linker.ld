/* x86_64 higher-half kernel.
 *
 * LMA (physical) starts at 1 MiB, where GRUB drops the multiboot image.
 * VMA (virtual) is in the top 2 GiB at 0xFFFFFFFF80000000 — the classic
 * -mcmodel=kernel negative-high-half so RIP-relative + sign-extended
 * 32-bit displacements reach every symbol.
 *
 * The .boot section stays in the low half (LMA == VMA) because it runs
 * in 32-bit protected mode before paging is on. Everything else uses
 * AT(...) to separate load from virtual address.
 */

ENTRY(_start)
OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)

KERNEL_VMA = 0xFFFFFFFF80000000;
KERNEL_LMA = 0x00100000;

SECTIONS
{
    . = KERNEL_LMA;
    _kernel_phys_start = .;

    /* 32-bit trampoline + multiboot header — must be identity-mapped
     * because the CPU is still in protected mode when it runs. */
    .boot ALIGN(4K) :
    {
        KEEP(*(.multiboot))
        *(.boot)
        *(.boot.*)
    }

    . = ALIGN(4K);
    _boot_end = .;

    /* Higher-half image: LMA is just past .boot, VMA jumps into the
     * negative top-2GiB. */
    . = KERNEL_VMA + _boot_end;
    _kernel_start = .;

    .text ALIGN(4K) : AT(ADDR(.text) - KERNEL_VMA)
    {
        *(.text)
        *(.text.*)
    }

    .rodata ALIGN(4K) : AT(ADDR(.rodata) - KERNEL_VMA)
    {
        *(.rodata)
        *(.rodata.*)
    }

    .data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VMA)
    {
        *(.data)
        *(.data.*)
    }

    .bss ALIGN(4K) : AT(ADDR(.bss) - KERNEL_VMA)
    {
        *(COMMON)
        *(.bss)
        *(.bss.*)
    }

    _kernel_end = .;
    _kernel_phys_end = . - KERNEL_VMA;
}
