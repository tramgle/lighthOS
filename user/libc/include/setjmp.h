#ifndef _SETJMP_H
#define _SETJMP_H

/* x86_64 SysV: { rbx, rbp, r12, r13, r14, r15, rsp, rip } */
typedef long jmp_buf[8];

int  setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val);

#endif
