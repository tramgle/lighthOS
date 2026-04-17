; ld-lighthos.so.1 entry. The kernel lays out a SysV AMD64 stack:
;   [rsp+0]        argc
;   [rsp+8]        argv[0]
;   ...
;   [rsp+8+8*argc] NULL (argv terminator)
;                  envp pointers ... NULL
;                  auxv pairs (key, val) ... (AT_NULL, 0)
;
; We forward that stack pointer to ld_main which returns main's
; entry in RAX. The stack top is unchanged so main's own _start
; still sees argc at [rsp].

bits 64
section .text
global _start
extern ld_main

_start:
    mov rdi, rsp            ; first arg: pointer to argc
    call ld_main            ; returns main's entry in rax
    jmp rax                 ; transfer to main's _start
