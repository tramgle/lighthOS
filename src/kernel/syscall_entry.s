; x86_64 SYSCALL entry.
;
; The CPU enters here from user space after executing `syscall`.
; State on entry:
;   RCX  = user RIP (saved by the CPU)
;   R11  = user RFLAGS (saved by the CPU)
;   RAX  = syscall number
;   RDI,RSI,RDX,R10,R8,R9 = args (SysV AMD64 syscall convention)
;   RSP  = user stack (CPU does NOT switch stacks on syscall)
;   RFLAGS has IF cleared because SFMASK masks it.
;
; Because RFLAGS.IF is cleared and we're on UP hardware, it's safe
; to stash the user RSP in a module-local scratch word while we
; switch to the kernel stack. That avoids needing swapgs + a
; per-CPU GS block for the tiny prologue.
;
; We build a frame shaped exactly like registers_t (i.e. the same
; layout isr_stub.s lays down on an interrupt) so:
;   - syscall_handler can consume it unchanged, and
;   - if the handler yields, the other task's iretq return path
;     can pop/iretq our frame just as if we had come in through
;     the interrupt gate.
;
; Return path: pop GPRs, drop int_no+err, iretq. Same as isr_common.

bits 64

extern syscall_handler
global syscall_entry_64
global syscall_kernel_rsp

section .bss
align 16
syscall_kernel_rsp: resq 1       ; current task's top-of-kernel-stack
scratch_user_rsp:   resq 1       ; stash while switching stacks

section .text

syscall_entry_64:
    mov [rel scratch_user_rsp], rsp
    mov rsp, [rel syscall_kernel_rsp]

    ; Build iretq-compatible CPU frame on the kernel stack.
    push qword 0x23                  ; SS  = user data | RPL3
    push qword [rel scratch_user_rsp]
    push r11                         ; user RFLAGS
    push qword 0x2B                  ; CS  = user code | RPL3
    push rcx                         ; user RIP

    ; Fake err_code + int_no so registers_t matches ISR layout.
    push qword 0                     ; err_code
    push qword 0x80                  ; int_no — "came from syscall"

    ; GPRs in the same push order as isr_common.
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rbp
    push rdi
    push rsi
    push rdx
    push rcx
    push rbx
    push rax

    mov rdi, rsp
    call syscall_handler
    mov rsp, rax                     ; schedule may hand back a different frame

    pop rax
    pop rbx
    pop rcx
    pop rdx
    pop rsi
    pop rdi
    pop rbp
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15

    add rsp, 16                      ; drop int_no + err_code
    iretq
