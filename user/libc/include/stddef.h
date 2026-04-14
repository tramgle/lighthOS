#ifndef _STDDEF_H
#define _STDDEF_H

#ifndef _SIZE_T_DEFINED
#define _SIZE_T_DEFINED
typedef unsigned int size_t;
#endif

typedef int ssize_t;
typedef int ptrdiff_t;
typedef int wchar_t;

#ifndef NULL
#define NULL ((void *)0)
#endif

#define offsetof(t, m) __builtin_offsetof(t, m)

#endif
