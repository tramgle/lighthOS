global _start
extern main
extern environ

section .text
_start:
    ; SysV stack layout at entry:
    ;   [esp+0]             = argc
    ;   [esp+4]             = argv[0]
    ;   ...
    ;   [esp+4+4*argc]      = NULL       (argv terminator)
    ;   [esp+8+4*argc]      = envp[0]    (or NULL if no env)
    ;   ...                   NULL       (envp terminator)
    ;                         auxv pairs, AT_NULL, 0
    ;
    ; Extract argc, argv, envp. Store envp in ulib's `environ` so
    ; getenv/setenv see it. Call main(argc, argv, envp) — SysV calling
    ; convention for user programs that want to peek at env.
    mov eax, [esp]                    ; argc
    lea edx, [esp + 4]                ; argv = &argv[0]
    lea ecx, [edx + eax * 4 + 4]      ; envp = argv + (argc+1)*4
    mov [environ], ecx
    push ecx                          ; third arg: envp
    push edx                          ; second arg: argv
    push eax                          ; first arg: argc
    call main
    ; sys_exit(return value)
    mov ebx, eax
    mov eax, 1
    int 0x80
    jmp $
