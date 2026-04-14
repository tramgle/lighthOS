global _start
extern main

section .text
_start:
    ; Kernel sets up [esp]=argc, [esp+4]=argv before jumping to us.
    ; Convert to the standard C calling convention by pushing argv then
    ; argc onto the stack before calling main(argc, argv).
    mov eax, [esp]          ; argc
    mov edx, [esp + 4]      ; argv
    push edx                ; second arg: argv
    push eax                ; first arg: argc
    call main
    ; sys_exit(return value)
    mov ebx, eax            ; exit code = main's return value
    mov eax, 1              ; SYS_EXIT
    int 0x80
    ; should not reach here
    jmp $
