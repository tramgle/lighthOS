#ifndef _STDIO_H
#define _STDIO_H

#include <stddef.h>
#include <stdarg.h>

#define EOF      (-1)
#define BUFSIZ   1024
#define L_tmpnam 32
#define FOPEN_MAX 16
#define FILENAME_MAX 256
#define TMP_MAX  16

#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

typedef long fpos_t;
typedef struct FILE_s FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

FILE  *fopen(const char *path, const char *mode);
FILE  *freopen(const char *path, const char *mode, FILE *f);
int    fclose(FILE *f);
int    fflush(FILE *f);
size_t fread(void *dst, size_t size, size_t nmemb, FILE *f);
size_t fwrite(const void *src, size_t size, size_t nmemb, FILE *f);

int    fgetc(FILE *f);
int    getc(FILE *f);
int    getchar(void);
int    ungetc(int c, FILE *f);
char  *fgets(char *out, int size, FILE *f);
int    fputc(int c, FILE *f);
int    putc(int c, FILE *f);
int    putchar(int c);
int    fputs(const char *s, FILE *f);
int    puts(const char *s);

int    fseek(FILE *f, long offset, int whence);
long   ftell(FILE *f);
void   rewind(FILE *f);
int    fgetpos(FILE *f, fpos_t *pos);
int    fsetpos(FILE *f, const fpos_t *pos);

int    feof(FILE *f);
int    ferror(FILE *f);
void   clearerr(FILE *f);
int    fileno(FILE *f);
int    setvbuf(FILE *f, char *buf, int mode, size_t size);

FILE  *tmpfile(void);
char  *tmpnam(char *buf);
FILE  *popen(const char *cmd, const char *mode);
int    pclose(FILE *f);
int    remove(const char *path);
int    rename(const char *from, const char *to);

int    printf(const char *fmt, ...);
int    fprintf(FILE *f, const char *fmt, ...);
int    sprintf(char *out, const char *fmt, ...);
int    snprintf(char *out, size_t size, const char *fmt, ...);
int    vprintf(const char *fmt, va_list ap);
int    vfprintf(FILE *f, const char *fmt, va_list ap);
int    vsprintf(char *out, const char *fmt, va_list ap);
int    vsnprintf(char *out, size_t size, const char *fmt, va_list ap);

#endif
