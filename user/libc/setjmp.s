; 32-bit setjmp/longjmp — cdecl.
;
; Layout of jmp_buf (uint32_t[6]): ebx, esi, edi, ebp, esp, eip
;
; setjmp(env):
;   Save callee-preserved regs + caller's esp (after return addr pop)
;   + return address. Returns 0 on direct call.
;
; longjmp(env, val):
;   Restore saved regs, rewrite the return address on the new stack so
;   we "return" to where setjmp was called from, and put val (or 1 if
;   val == 0) into eax.

bits 32

; `:function` makes nasm tag these as STT_FUNC in the ELF symbol
; table. Without it the defaults to NOTYPE, and ld-lighthos.so.1 skips
; NOTYPE symbols during interposition — see lookup_in in
; user/ldso/ld_main.c.
global setjmp:function
global longjmp:function

setjmp:
    mov eax, [esp + 4]    ; env
    mov [eax + 0],  ebx
    mov [eax + 4],  esi
    mov [eax + 8],  edi
    mov [eax + 12], ebp
    lea ecx, [esp + 4]    ; caller's esp (above return address)
    mov [eax + 16], ecx
    mov ecx, [esp]        ; return address
    mov [eax + 20], ecx
    xor eax, eax
    ret

longjmp:
    mov edx, [esp + 4]    ; env
    mov eax, [esp + 8]    ; val
    test eax, eax
    jnz .have_val
    mov eax, 1
.have_val:
    mov ebx, [edx + 0]
    mov esi, [edx + 4]
    mov edi, [edx + 8]
    mov ebp, [edx + 12]
    mov esp, [edx + 16]
    mov ecx, [edx + 20]
    jmp ecx
