#include "ulib.h"

typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_end(ap)         __builtin_va_end(ap)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *(unsigned char *)a - *(unsigned char *)b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (a[i] == '\0') return 0;
    }
    return 0;
}

char *strncpy(char *dest, const char *src, size_t n) {
    size_t i;
    for (i = 0; i < n && src[i]; i++) dest[i] = src[i];
    for (; i < n; i++) dest[i] = '\0';
    return dest;
}

char *strcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++));
    return dest;
}

char *strcat(char *dest, const char *src) {
    char *d = dest + strlen(dest);
    while ((*d++ = *src++));
    return dest;
}

void *memset(void *s, int c, size_t n) {
    unsigned char *p = (unsigned char *)s;
    while (n--) *p++ = (unsigned char)c;
    return s;
}

void *memcpy(void *dest, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
    return dest;
}

void putchar(char c) {
    sys_write(1, &c, 1);
}

void puts(const char *s) {
    sys_write(1, s, strlen(s));
}

/* Render `val` in `base` to `buf` (at most 32 chars). Returns length. */
static int uint_to_buf(uint32_t val, int base, char *buf) {
    const char *digits = "0123456789abcdef";
    int i = 0;
    if (val == 0) { buf[i++] = '0'; return i; }
    while (val > 0) {
        buf[i++] = digits[val % base];
        val /= base;
    }
    return i;
}

static void emit_padded(const char *buf, int len, int width, int left_align, char pad) {
    if (!left_align) {
        for (int i = len; i < width; i++) putchar(pad);
    }
    for (int i = 0; i < len; i++) putchar(buf[i]);
    if (left_align) {
        for (int i = len; i < width; i++) putchar(' ');
    }
}

int printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int count = 0;

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            putchar(*fmt);
            count++;
            continue;
        }
        fmt++;

        /* Flags: '-' = left align. */
        int left_align = 0;
        if (*fmt == '-') { left_align = 1; fmt++; }

        /* Pad char: '0' = zero-pad, else space. */
        char pad = ' ';
        if (*fmt == '0') { pad = '0'; fmt++; }

        /* Width: digits. */
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        char tmp[32];
        int len = 0;

        switch (*fmt) {
        case 'd': {
            int32_t v = va_arg(args, int32_t);
            int neg = 0;
            uint32_t u;
            if (v < 0) { neg = 1; u = (uint32_t)(-v); }
            else u = (uint32_t)v;
            len = uint_to_buf(u, 10, tmp);
            /* Reverse in place since uint_to_buf writes low digit first. */
            for (int i = 0, j = len - 1; i < j; i++, j--) {
                char t = tmp[i]; tmp[i] = tmp[j]; tmp[j] = t;
            }
            if (neg) {
                /* Shift right and prepend '-'. */
                for (int i = len; i > 0; i--) tmp[i] = tmp[i - 1];
                tmp[0] = '-';
                len++;
            }
            emit_padded(tmp, len, width, left_align, pad);
            break;
        }
        case 'u':
        case 'x': {
            uint32_t u = va_arg(args, uint32_t);
            len = uint_to_buf(u, (*fmt == 'x') ? 16 : 10, tmp);
            for (int i = 0, j = len - 1; i < j; i++, j--) {
                char t = tmp[i]; tmp[i] = tmp[j]; tmp[j] = t;
            }
            emit_padded(tmp, len, width, left_align, pad);
            break;
        }
        case 's': {
            const char *s = va_arg(args, const char *);
            if (!s) s = "(null)";
            int slen = 0;
            while (s[slen]) slen++;
            emit_padded(s, slen, width, left_align, ' ');
            break;
        }
        case 'c': putchar((char)va_arg(args, int)); break;
        case '%': putchar('%'); break;
        case '\0': va_end(args); return count;
        default: putchar('%'); putchar(*fmt); break;
        }
    }
    va_end(args);
    return count;
}
