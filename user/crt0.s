global _start
extern main

section .text
_start:
    ; SysV stack layout at entry:
    ;   [esp+0]        = argc
    ;   [esp+4]        = argv[0]   (first element of the argv array)
    ;   ...
    ;   [esp+4+4*argc] = NULL       (argv terminator)
    ;                    envp NULL, then auxv pairs, then AT_NULL,0
    ; Compute argv as the address of argv[0] (lea, not mov), then
    ; push argv/argc for the C calling convention and call main.
    mov eax, [esp]          ; argc
    lea edx, [esp + 4]      ; argv = &argv[0]
    push edx                ; second arg: argv
    push eax                ; first arg: argc
    call main
    ; sys_exit(return value)
    mov ebx, eax            ; exit code = main's return value
    mov eax, 1              ; SYS_EXIT
    int 0x80
    ; should not reach here
    jmp $
