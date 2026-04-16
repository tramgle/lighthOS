#ifndef ULIB_H
#define ULIB_H

#include "syscall.h"

typedef unsigned int size_t;

size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strncpy(char *dest, const char *src, size_t n);
char  *strcpy(char *dest, const char *src);
char  *strcat(char *dest, const char *src);
void  *memset(void *s, int c, size_t n);
void  *memcpy(void *dest, const void *src, size_t n);

void   puts(const char *s);
void   putchar(char c);
int    printf(const char *fmt, ...);

/* Signal dispositions. Match POSIX. */
#define SIG_DFL ((void (*)(int)) 0)
#define SIG_IGN ((void (*)(int)) 1)
#define SIG_ERR ((void (*)(int)) -1)
#define NSIG_USER 32
typedef void (*sighandler_t)(int);

/* Install a user-level signal handler. Stores `handler` in a per-
   process table and installs a shared trampoline with the kernel; when
   the kernel delivers `signo`, the trampoline looks up and invokes
   `handler`, then unconditionally calls sys_sigreturn. Returns the
   previous handler (SIG_DFL/SIG_IGN/fn); SIG_ERR on invalid signo or
   uncatchable signal (SIG_KILL, SIG_STOP). */
sighandler_t signal(int signo, sighandler_t handler);

#endif
