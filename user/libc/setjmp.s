; x86_64 setjmp / longjmp.
; jmp_buf layout (uint64_t[8]): rbx, rbp, r12, r13, r14, r15, rsp, rip.

bits 64
global setjmp:function
global longjmp:function

setjmp:
    mov [rdi + 0],  rbx
    mov [rdi + 8],  rbp
    mov [rdi + 16], r12
    mov [rdi + 24], r13
    mov [rdi + 32], r14
    mov [rdi + 40], r15
    lea rcx, [rsp + 8]              ; caller's rsp (after the call pushed retaddr)
    mov [rdi + 48], rcx
    mov rcx, [rsp]                  ; return address
    mov [rdi + 56], rcx
    xor eax, eax
    ret

longjmp:
    ; longjmp(env, val) — env in rdi, val in rsi.
    mov eax, esi
    test eax, eax
    jnz .have
    mov eax, 1
.have:
    mov rbx, [rdi + 0]
    mov rbp, [rdi + 8]
    mov r12, [rdi + 16]
    mov r13, [rdi + 24]
    mov r14, [rdi + 32]
    mov r15, [rdi + 40]
    mov rsp, [rdi + 48]
    mov rcx, [rdi + 56]
    jmp rcx
