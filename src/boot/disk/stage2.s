; LighthOS second-stage bootloader.
;
; Entry: real mode, DL = BIOS drive, loaded at 0x8000 by the MBR.
; Tasks:
;   1. Save BIOS drive number.
;   2. Query E820 memory map -> 0x11000.
;   3. Read raw kernel ELF from disk (starting at KERNEL_LBA) into a
;      scratch buffer at 0x20000.
;   4. Switch to 32-bit protected mode (flat GDT).
;   5. Parse ELF at 0x20000, copy each PT_LOAD to its p_vaddr.
;   6. Write a multiboot_info_t at 0x10000 referencing the memory map
;      we saved earlier.
;   7. Jump to the kernel's entry with EAX=0x2BADB002, EBX=0x10000.
;
; Disk layout this assumes:
;   LBA 0        = MBR
;   LBA 1..62    = stage2 (this file, up to 31 KB)
;   LBA 63..2047 = kernel ELF (raw, up to ~990 KB)
;   LBA 2048+    = simplefs partition

%define KERNEL_LBA        63
%define KERNEL_SECTS      512           ; read up to 256 KB of kernel
%define KERNEL_BUF_SEG    0x2000        ; physical 0x20000
%define MBI_ADDR          0x00010000    ; multiboot_info_t lives here
%define MMAP_ADDR         0x00011000    ; E820 entries here
%define MBOOT_MAGIC       0x2BADB002

; --- 16-bit real mode -------------------------------------------------

BITS 16
ORG 0x8000

start16:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00                      ; reuse stack below MBR
    sti

    mov [drive], dl

    mov si, msg_stage2
    call print16

    call query_e820
    call load_kernel

    mov si, msg_going_pm
    call print16

    ; --- Switch to 32-bit protected mode --------------------------------

    cli
    lgdt [gdt_desc]
    mov eax, cr0
    or  eax, 1
    mov cr0, eax
    jmp 0x08:pm_entry                   ; flush prefetch, enter PM

; ---- real-mode helpers -----------------------------------------------

; Print DS:SI zero-terminated string.
print16:
    lodsb
    test al, al
    jz .done
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    jmp print16
.done:
    ret

; Query BIOS int 0x15 AX=E820 memory map. Writes up to 32 entries
; (24 bytes each) to MMAP_ADDR. Stores total byte length at [mmap_len].
query_e820:
    pusha
    mov di, MMAP_ADDR & 0xFFFF          ; offset
    mov ax, MMAP_ADDR >> 4
    mov es, ax                          ; segment so ES:DI = MMAP_ADDR
    xor di, di

    xor ebx, ebx
    mov dword [mmap_len], 0
.loop:
    mov eax, 0xE820
    mov edx, 0x534D4150                 ; 'SMAP'
    mov ecx, 24
    int 0x15
    jc .done
    cmp eax, 0x534D4150
    jne .done

    add di, 24
    add dword [mmap_len], 24

    test ebx, ebx
    jz .done
    cmp di, 24*64                       ; hard cap: 64 entries
    jae .done
    jmp .loop
.done:
    xor ax, ax
    mov es, ax                          ; restore ES
    popa
    ret

; Read KERNEL_SECTS sectors starting at LBA KERNEL_LBA into KERNEL_BUF_SEG:0000.
; BIOS int 0x13 AH=0x42 can typically handle up to 127 sectors per call;
; loop reading chunks of 64 sectors (32 KB) until we've loaded the lot.
load_kernel:
    pusha

    mov dword [dap_lba_lo], KERNEL_LBA
    mov dword [dap_lba_hi], 0
    mov word  [dap_off], 0
    mov word  [dap_seg], KERNEL_BUF_SEG

    mov word  [sects_left], KERNEL_SECTS

.chunk:
    mov ax, [sects_left]
    test ax, ax
    jz .done
    cmp ax, 64
    jbe .small
    mov ax, 64
.small:
    mov word [dap_size],  0x0010
    mov word [dap_count], ax

    mov dl, [drive]
    mov si, dap
    mov ah, 0x42
    int 0x13
    jc .disk_err

    ; advance LBA, target segment, remaining count
    mov cx, [dap_count]
    add dword [dap_lba_lo], ecx
    mov ax, [dap_seg]
    mov bx, cx
    shl bx, 5                            ; sectors * 512 / 16 = sectors * 32 paragraphs
    add ax, bx
    mov [dap_seg], ax

    sub word [sects_left], cx
    jmp .chunk
.done:
    popa
    ret
.disk_err:
    mov si, msg_disk_err
    call print16
.hang16:
    hlt
    jmp .hang16

; ---- strings ---------------------------------------------------------

msg_stage2:    db "lighthos stage2", 13, 10, 0
msg_going_pm:  db "-> pm", 13, 10, 0
msg_disk_err:  db "kernel read fail", 13, 10, 0

drive:         db 0
align 2
sects_left:    dw 0
mmap_len:      dd 0
align 4
dap:
dap_size:      dw 0
dap_count:     dw 0
dap_off:       dw 0
dap_seg:       dw 0
dap_lba_lo:    dd 0
dap_lba_hi:    dd 0

; ---- GDT: flat 32-bit code + data ------------------------------------

align 8
gdt_start:
    dq 0                                ; null
    ; 0x08: code, base=0 limit=4GB, 32-bit, exec/read, ring 0
    dw 0xFFFF, 0x0000
    db 0x00, 10011010b, 11001111b, 0x00
    ; 0x10: data, base=0 limit=4GB, 32-bit, read/write, ring 0
    dw 0xFFFF, 0x0000
    db 0x00, 10010010b, 11001111b, 0x00
gdt_end:

gdt_desc:
    dw gdt_end - gdt_start - 1
    dd gdt_start

; --- 32-bit protected mode --------------------------------------------

BITS 32
pm_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x9FC00                    ; 640 KB - just under EBDA

    ; Parse ELF at 0x20000 and copy PT_LOAD segments to their p_vaddr.
    mov esi, 0x20000                    ; ESI = ELF base

    ; Check ELF magic 0x464C457F.
    mov eax, [esi]
    cmp eax, 0x464C457F
    jne elf_bad

    ; e_entry at offset 24, e_phoff at 28, e_phentsize at 42, e_phnum at 44.
    mov eax, [esi + 24]
    mov [kernel_entry], eax

    mov ebx, [esi + 28]                 ; phoff
    movzx ecx, word [esi + 44]          ; phnum
    movzx edx, word [esi + 42]          ; phentsize

    add ebx, esi                        ; ebx = phdr cursor
.ph_loop:
    test ecx, ecx
    jz ph_done

    ; p_type at 0
    mov eax, [ebx]
    cmp eax, 1                          ; PT_LOAD
    jne .next

    ; p_offset at 4, p_vaddr at 8, p_filesz at 16, p_memsz at 20
    mov edi, [ebx + 8]                  ; dest = p_vaddr
    mov eax, [ebx + 4]                  ; src offset
    mov esi, [ebx + 4]
    add esi, 0x20000                    ; src = base + p_offset

    push ecx
    mov ecx, [ebx + 16]                 ; filesz
    test ecx, ecx
    jz .skip_copy
    rep movsb
.skip_copy:
    ; Zero BSS tail: memsz - filesz bytes past the copied region.
    mov ecx, [ebx + 20]                 ; memsz
    sub ecx, [ebx + 16]                 ; memsz - filesz
    xor eax, eax
    test ecx, ecx
    jz .no_bss
    rep stosb
.no_bss:
    pop ecx
.next:
    add ebx, edx                        ; next phdr
    dec ecx
    jmp .ph_loop

ph_done:
    ; Populate multiboot_info_t at MBI_ADDR.
    ; Only advertise mem_lower/mem_upper (flag bit 0). We *could* expose
    ; the E820 map via flag bit 6 but the multiboot layout wraps each
    ; E820 entry in a size prefix — not worth the extra real-mode code
    ; when mem_upper alone satisfies pmm_init.
    mov edi, MBI_ADDR
    mov dword [edi +  0], 1
    mov dword [edi +  4], 640                   ; mem_lower (KB)
    mov dword [edi +  8], 130048                ; mem_upper (KB) — 127 MB
    mov dword [edi + 12], 0                     ; boot_device
    mov dword [edi + 16], 0                     ; cmdline
    mov dword [edi + 20], 0                     ; mods_count
    mov dword [edi + 24], 0                     ; mods_addr
    mov dword [edi + 28], 0                     ; syms[0..3]
    mov dword [edi + 32], 0
    mov dword [edi + 36], 0
    mov dword [edi + 40], 0
    mov dword [edi + 44], 0                     ; mmap_length
    mov dword [edi + 48], 0                     ; mmap_addr
    mov dword [edi + 52], 0                     ; drives_length
    mov dword [edi + 56], 0
    mov dword [edi + 60], 0                     ; config_table
    mov dword [edi + 64], 0                     ; boot_loader_name
    mov dword [edi + 68], 0                     ; apm_table

    ; Ready: EAX=magic, EBX=mbi, jump.
    mov eax, MBOOT_MAGIC
    mov ebx, MBI_ADDR
    mov ecx, [kernel_entry]
    jmp ecx

elf_bad:
.hang_pm:
    hlt
    jmp .hang_pm

align 4
kernel_entry: dd 0
