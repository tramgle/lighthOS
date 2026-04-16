; x86_64 user crt0. SysV AMD64 ABI.
;
; Kernel (process.c:build_user_stack) leaves the stack at entry as:
;   [rsp+ 0]      argc
;   [rsp+ 8]      argv[0]
;   ...
;   [rsp+8*N]     argv[argc] = NULL
;   [rsp+...]     envp NULL
;   [rsp+...]     auxv pairs + AT_NULL
;
; SysV AMD64: argc→RDI, argv→RSI, envp→RDX before calling main.
; main's prologue expects (rsp % 16) == 8 on entry (because the
; caller's `call` pushed a return address onto an already-16-
; aligned stack). The kernel lays out `sp` as 16-aligned, so after
; our `call main` the invariant holds.

bits 64
section .text

extern main
global _start

_start:
    mov rdi, [rsp]                   ; argc
    lea rsi, [rsp + 8]               ; argv
    mov rcx, rdi
    lea rdx, [rsi + rcx*8 + 8]       ; envp = argv + (argc+1)*8
    xor rbp, rbp                     ; clean frame-pointer for backtraces
    call main
    mov rdi, rax                     ; exit code = main's return
    mov rax, 1                       ; SYS_EXIT
    int 0x80
.hang:
    jmp .hang
