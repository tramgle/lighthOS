#ifndef _STDLIB_H
#define _STDLIB_H

#include <stddef.h>

#ifndef EXIT_SUCCESS
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#endif

#define RAND_MAX 0x7fffffff

void  *malloc(size_t size);
void  *calloc(size_t n, size_t sz);
void  *realloc(void *p, size_t size);
void   free(void *p);

double strtod(const char *s, char **endp);
long   strtol(const char *s, char **endp, int base);
unsigned long strtoul(const char *s, char **endp, int base);
long long strtoll(const char *s, char **endp, int base);
unsigned long long strtoull(const char *s, char **endp, int base);

int    atoi(const char *s);
long   atol(const char *s);
double atof(const char *s);

void   exit(int status);
void   abort(void);
int    atexit(void (*fn)(void));
char  *getenv(const char *name);
int    system(const char *cmd);

int    rand(void);
void   srand(unsigned seed);

void   qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));

int       abs(int x);
long      labs(long x);
long long llabs(long long x);

typedef struct { int quot; int rem; } div_t;
typedef struct { long quot; long rem; } ldiv_t;
div_t  div(int num, int den);
ldiv_t ldiv(long num, long den);

#endif
