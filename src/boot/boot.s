; x86_64 boot entry for LighthOS.
;
; GRUB loads us via multiboot1 in 32-bit protected mode. This file:
;   1. Saves multiboot magic + info pointer in callee-saved GPRs.
;   2. Builds a bootstrap 4-level page table with:
;        - low 1 GiB identity-mapped (so code can keep running
;          immediately after paging turns on),
;        - same 1 GiB also mapped at the -mcmodel=kernel higher-half
;          VMA 0xFFFFFFFF80000000.
;   3. Enables PAE, sets EFER.LME, loads CR3, enables paging.
;   4. Loads a bootstrap 64-bit GDT and far-jumps into a 64-bit code
;      segment.
;   5. Reloads data selectors, switches to the higher-half kernel
;      stack, calls kernel_main(magic, mbi).
;
; Bootstrap page tables use 2 MiB huge pages (PDE.PS=1). Physical
; 0..1 GiB is plenty for bringup; L2's real VMM will rebuild from
; scratch with finer granularity.
;
; The .boot section is identity-mapped (LMA == VMA, low half), so
; all 32-bit code + data lives there. The rest of the kernel lives
; in the higher-half .text/.rodata/.data/.bss.

; ---- Multiboot1 header (first 8 KiB of the image) ----------------
MBOOT_MAGIC     equ 0x1BADB002
MBOOT_FLAGS     equ 0x00000003          ; align modules + memory map
MBOOT_CHECKSUM  equ -(MBOOT_MAGIC + MBOOT_FLAGS)

section .multiboot
align 4
    dd MBOOT_MAGIC
    dd MBOOT_FLAGS
    dd MBOOT_CHECKSUM

; ---- Bootstrap page tables (in .boot — identity-mapped) -----------
section .boot
align 4096
boot_pml4:
    times 512 dq 0
align 4096
boot_pdpt_low:
    times 512 dq 0
align 4096
boot_pdpt_high:
    times 512 dq 0
align 4096
boot_pd:
    times 512 dq 0

; Bootstrap stack used *only* during 32-bit trampoline. Lives in
; .boot so its LMA matches its VMA (no higher-half pointer needed
; before paging is on).
align 16
boot_stack_bottom:
    times 4096 db 0
boot_stack_top:

; ---- Real kernel stack (higher-half .bss) ------------------------
section .bss
align 16
kernel_stack_bottom:
    resb 16384                          ; 16 KiB
global stack_top
stack_top:

; ---- 32-bit trampoline ------------------------------------------
section .boot
bits 32

PRESENT     equ 1
WRITE       equ 2
HUGE_PAGE   equ (1 << 7)
EFER_MSR    equ 0xC0000080
EFER_LME    equ (1 << 8)
CR0_PG      equ (1 << 31)
CR4_PAE     equ (1 << 5)

global _start
extern kernel_main

_start:
    cli
    mov esp, boot_stack_top

    ; Save multiboot magic (eax) + info pointer (ebx) in regs that
    ; survive the pmode→lmode transition.
    mov edi, eax                        ; magic
    mov esi, ebx                        ; mbi pointer (phys)

    call build_paging_tables

    ; Install CR3, enable PAE, set EFER.LME, enable paging.
    mov eax, boot_pml4
    mov cr3, eax

    mov eax, cr4
    or  eax, CR4_PAE
    mov cr4, eax

    mov ecx, EFER_MSR
    rdmsr
    or  eax, EFER_LME
    wrmsr

    mov eax, cr0
    or  eax, CR0_PG
    mov cr0, eax

    ; Long mode is now active (in 32-bit compatibility sub-mode).
    ; Load the 64-bit GDT + far-jump into 64-bit code.
    lgdt [boot_gdt_ptr]
    jmp 0x08:long_mode_entry

build_paging_tables:
    ; PML4[0] → PDPT_LOW (identity-map low half)
    mov eax, boot_pdpt_low
    or  eax, PRESENT | WRITE
    mov [boot_pml4 + 0*8], eax
    mov dword [boot_pml4 + 0*8 + 4], 0

    ; PML4[256] → PDPT_LOW (HHDM at 0xFFFF800000000000).
    ; vmm_init installs the same entry in the new kernel PML4 so
    ; kernel code has unbroken access to physical memory through
    ; the CR3 switch.
    mov eax, boot_pdpt_low
    or  eax, PRESENT | WRITE
    mov [boot_pml4 + 256*8], eax
    mov dword [boot_pml4 + 256*8 + 4], 0

    ; PML4[511] → PDPT_HIGH (higher-half kernel at -2 GiB)
    mov eax, boot_pdpt_high
    or  eax, PRESENT | WRITE
    mov [boot_pml4 + 511*8], eax
    mov dword [boot_pml4 + 511*8 + 4], 0

    ; PDPT_LOW[0] → boot_pd (identity map 0..1 GiB)
    mov eax, boot_pd
    or  eax, PRESENT | WRITE
    mov [boot_pdpt_low + 0*8], eax
    mov dword [boot_pdpt_low + 0*8 + 4], 0

    ; PDPT_HIGH[510] → boot_pd
    ;   VMA 0xFFFFFFFF80000000 = PML4[511], PDPT[510], PD[0..511]
    mov [boot_pdpt_high + 510*8], eax
    mov dword [boot_pdpt_high + 510*8 + 4], 0

    ; Fill boot_pd with 512 × 2 MiB huge pages = 1 GiB identity map.
    xor ecx, ecx
.fill_pd:
    mov eax, ecx
    shl eax, 21                         ; 2 MiB per entry
    or  eax, PRESENT | WRITE | HUGE_PAGE
    mov [boot_pd + ecx*8], eax
    mov dword [boot_pd + ecx*8 + 4], 0
    inc ecx
    cmp ecx, 512
    jb  .fill_pd
    ret

; ---- Bootstrap 64-bit GDT ---------------------------------------
align 8
boot_gdt:
    dq 0x0000000000000000               ; 0x00: null
    dq 0x00AF9A000000FFFF               ; 0x08: kernel code (L=1, DPL=0)
    dq 0x00CF92000000FFFF               ; 0x10: kernel data (ignored in LM)
boot_gdt_end:

boot_gdt_ptr:
    dw boot_gdt_end - boot_gdt - 1
    dq boot_gdt

; ---- 64-bit continuation (still in .boot, runs in long mode) ----
bits 64
long_mode_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; Enable SSE so user-space compilers can return doubles via
    ; XMM0 (ABI). CR0: clear EM (bit 2), set MP (bit 1).
    ; CR4: set OSFXSR (bit 9) + OSXMMEXCPT (bit 10).
    mov rax, cr0
    and rax, ~(1 << 2)                  ; clear EM
    or  rax, (1 << 1)                   ; set MP
    mov cr0, rax
    mov rax, cr4
    or  rax, (1 << 9) | (1 << 10)       ; OSFXSR + OSXMMEXCPT
    mov cr4, rax

    ; Jump to higher-half VMA for the rest of the kernel.
    mov rax, higher_half_entry
    jmp rax

; ---- Higher-half entry (lives in .text at KERNEL_VMA) -----------
section .text
bits 64
higher_half_entry:
    mov rsp, stack_top                  ; switch to real kernel stack

    ; Args to kernel_main: magic in RDI, mbi pointer in RSI.
    ; We saved magic→EDI and mbi→ESI pre-transition; both are
    ; already in the low 32 bits of RDI/RSI (upper bits zero-
    ; extended on pmode writes, preserved across mode switch).
    xor rbp, rbp                        ; clean backtrace root
    call kernel_main

    cli
.hang:
    hlt
    jmp .hang
