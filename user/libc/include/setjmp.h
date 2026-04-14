#ifndef _SETJMP_H
#define _SETJMP_H

/* { ebx, esi, edi, ebp, esp, eip } */
typedef int jmp_buf[6];

int  setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val);

#endif
