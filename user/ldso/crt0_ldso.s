; ld-vibeos.so.1 entry point.
;
; The kernel hands us a SysV-style stack:
;   [esp+0]        argc
;   [esp+4]        argv[0]
;   ...
;   [esp+4+4*argc] NULL (argv terminator)
;                  envp NULL (empty environment for now)
;                  auxv pairs (type, value)
;                  AT_NULL, 0
;
; Our job:
;   1. Pass the whole stack pointer to ld_main(), which parses auxv,
;      loads DT_NEEDED libraries, applies relocations, and returns
;      the main executable's entry point in EAX.
;   2. Restore ESP to its entry value (so main's _start sees
;      [esp+0] = argc unchanged).
;   3. Jump to main's entry.
;
; Never return — main's own crt0 handles exit via SYS_EXIT.

global _start
extern ld_main

section .text
_start:
    push esp            ; arg: pointer to argc on the stack
    call ld_main        ; returns main exec's entry in EAX
    add esp, 4          ; undo the push; ESP back to &argc
    jmp eax             ; transfer to main's _start
