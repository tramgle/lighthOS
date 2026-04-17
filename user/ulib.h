/* ulib.h — legacy API surface, implemented by user/ulib.c (linked
 * via libulib.a) on top of the x86_64 syscall wrappers. */
#ifndef ULIB_H
#define ULIB_H

#include "syscall_x64.h"

/* Function prototypes — defined once in user/ulib.c. */
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strncpy(char *dest, const char *src, size_t n);
char  *strcpy(char *dest, const char *src);
char  *strcat(char *dest, const char *src);
void  *memset(void *s, int c, size_t n);
void  *memcpy(void *dest, const void *src, size_t n);
int    putchar(int c);
int    puts(const char *s);
int    printf(const char *fmt, ...);

#define SIG_ERR ((void (*)(int))-1)
#define NSIG_USER 32
typedef void (*sighandler_t)(int);
sighandler_t signal(int signo, sighandler_t handler);

extern char **environ;
char *getenv(const char *name);
int   setenv(const char *name, const char *value, int overwrite);
int   unsetenv(const char *name);

/* Heap-growth primitive used by libc/malloc.c. Backed by an
   mmap_anon arena that grows 64 KiB at a time. Defined in
   user/ulib.c (libulib.a). */
long sys_sbrk(long delta);

/* dlopen/dlsym deferred until ld.so port lands. */
#define RTLD_LAZY    0x1
#define RTLD_NOW     0x2
#define RTLD_GLOBAL  0x4
void       *dlopen(const char *path, int flags);
void       *dlsym(void *handle, const char *name);
int         dlclose(void *handle);
const char *dlerror(void);

#endif
