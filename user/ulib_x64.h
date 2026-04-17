/* Slim string helpers shared across the x86_64-ported user binaries.
 * Self-contained: only #includes syscall_x64.h (for size_t etc.). */
#ifndef ULIB_X64_H
#define ULIB_X64_H

#include "syscall_x64.h"

static inline size_t u_strlen(const char *s) {
    size_t n = 0; while (s[n]) n++; return n;
}
static inline int u_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
static inline int u_strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
        if (a[i] == 0) return 0;
    }
    return 0;
}
static inline void u_memcpy(void *dst, const void *src, size_t n) {
    char *d = dst; const char *s = src;
    while (n--) *d++ = *s++;
}
static inline void u_memset(void *dst, int c, size_t n) {
    char *d = dst; while (n--) *d++ = (char)c;
}
static inline int u_atoi(const char *s) {
    int sign = 1, v = 0;
    if (*s == '-') { sign = -1; s++; }
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return sign * v;
}
static inline void u_putc(char c)           { sys_write(1, &c, 1); }
static inline void u_puts_n(const char *s)  { sys_write(1, s, u_strlen(s)); }
static inline void u_putdec(long v) {
    char b[24]; int i = 0;
    if (v == 0) { u_putc('0'); return; }
    if (v < 0)  { u_putc('-'); v = -v; }
    while (v > 0) { b[i++] = '0' + (v % 10); v /= 10; }
    while (i > 0) u_putc(b[--i]);
}
/* Read a full file into `buf`. Returns bytes read or -1. */
static inline long u_slurp(const char *path, char *buf, size_t cap) {
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) return -1;
    long total = 0;
    while ((size_t)total < cap) {
        long n = sys_read(fd, buf + total, cap - total);
        if (n <= 0) break;
        total += n;
    }
    sys_close(fd);
    return total;
}
/* Copy a whole line from fd `fd` into `buf` (cap chars incl NUL);
   returns length not counting NUL, or 0 on EOF, -1 on error.
   Reads one byte at a time — simple, slow, fine for scripts. */
static inline long u_readline(int fd, char *buf, size_t cap) {
    size_t i = 0;
    while (i < cap - 1) {
        char c;
        long n = sys_read(fd, &c, 1);
        if (n < 0) return -1;
        if (n == 0) break;
        buf[i++] = c;
        if (c == '\n') break;
    }
    buf[i] = 0;
    return (long)i;
}

#endif
