; VibeOS MBR — stage 1 bootloader.
;
; Runs in 16-bit real mode at physical 0x7C00. DL = BIOS drive number.
; Loads STAGE2_SECTS sectors starting at LBA 1 into linear 0x8000, then
; jumps to 0x0000:0x8000 with DL preserved so stage2 knows the drive.
;
; 446 bytes code budget (rest of the 512-byte sector is the partition
; table + 0xAA55 signature). Uses int 0x13 AH=0x42 (LBA extensions) —
; QEMU and any modern BIOS support it, and it side-steps CHS geometry.

BITS 16
ORG 0x7C00

%define STAGE2_LOAD_SEG   0x0800    ; physical 0x8000
%define STAGE2_LOAD_OFF   0x0000
%define STAGE2_LBA        1
%define STAGE2_SECTS      62        ; ~31 KB budget for stage2

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00              ; stack just below us
    sti

    mov [drive], dl             ; remember BIOS drive

    mov si, msg_loading
    call print

    ; Build the Disk Address Packet on the stack (saves bytes vs .data).
    mov word [dap_size],  0x0010
    mov word [dap_count], STAGE2_SECTS
    mov word [dap_off],   STAGE2_LOAD_OFF
    mov word [dap_seg],   STAGE2_LOAD_SEG
    mov dword [dap_lba_lo], STAGE2_LBA
    mov dword [dap_lba_hi], 0

    mov ah, 0x42
    mov dl, [drive]
    mov si, dap
    int 0x13
    jc disk_err

    mov si, msg_jump
    call print

    mov dl, [drive]             ; stage2 expects DL = drive
    jmp STAGE2_LOAD_SEG:STAGE2_LOAD_OFF

disk_err:
    mov si, msg_err
    call print
.hang:
    hlt
    jmp .hang

; Print zero-terminated string at DS:SI via BIOS teletype.
print:
    lodsb
    test al, al
    jz .done
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    jmp print
.done:
    ret

msg_loading: db "vibeos mbr: loading stage2", 13, 10, 0
msg_jump:    db "ok", 13, 10, 0
msg_err:     db "disk read fail", 13, 10, 0

drive:       db 0

align 2
dap:
dap_size:    dw 0
dap_count:   dw 0
dap_off:     dw 0
dap_seg:     dw 0
dap_lba_lo:  dd 0
dap_lba_hi:  dd 0

; Pad to 446, then partition table + boot signature.
times 446-($-$$) db 0

; Partition 1: simplefs, LBA 2048..20479 (18432 sectors = 9 MB).
; Type 0x83 is arbitrary — our kernel looks at the MBR for layout only.
db 0x80                  ; status (bootable)
db 0, 0, 0               ; CHS start (unused)
db 0x83                  ; type: Linux native
db 0, 0, 0               ; CHS end
dd 2048                  ; LBA start
dd 18432                 ; LBA count

; Partition 2: FAT16, LBA 20480..32767 (12288 sectors = 6 MB).
db 0x00                  ; status
db 0, 0, 0               ; CHS start
db 0x06                  ; type: FAT16
db 0, 0, 0               ; CHS end
dd 20480                 ; LBA start
dd 12288                 ; LBA count

; Remaining 2 partition entries zero.
times 32 db 0

dw 0xAA55
