/* Minimal snprintf/vsnprintf supporting: %d %i %u %x %X %o %s %c %p %%.
 * Flags: '-' left-justify, '0' zero-pad, '+' signed, ' ' blank, '#' alt.
 * Width and .precision supported with numeric or '*' arg.
 * Length: 'l', 'll' (treats 'll' as 64-bit for %d/%u/%x only — Lua's
 * format needs can still work with our %d). 'z' for size_t.
 * No float support — Lua is configured LUA_32BITS integer-only.
 */

#include "ulib.h"

typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_end(ap)         __builtin_va_end(ap)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)

typedef struct {
    char *out;
    size_t pos;
    size_t cap;  /* capacity including final NUL */
} buf_t;

static void emit(buf_t *b, char c) {
    if (b->pos + 1 < b->cap) b->out[b->pos] = c;
    b->pos++;
}

static void emit_str(buf_t *b, const char *s, size_t n) {
    while (n--) emit(b, *s++);
}

static int utoa(unsigned long long val, int base, int upper, char *tmp) {
    const char *d_lo = "0123456789abcdef";
    const char *d_hi = "0123456789ABCDEF";
    const char *d = upper ? d_hi : d_lo;
    int i = 0;
    if (val == 0) { tmp[i++] = '0'; return i; }
    while (val) { tmp[i++] = d[val % base]; val /= base; }
    return i;
}

/* Write double `v` into `out` using a simplified %g / %e / %f formatter.
 * `prec` is the precision (number of significant digits for g, digits
 * after decimal for f/e). `conv` is 'g', 'e', or 'f'. Returns bytes
 * written. This is deliberately small — Lua uses it for `tostring(1.5)`
 * and similar; perfect round-tripping isn't required. */
static int dtoa(double v, char conv, int prec, char *out) {
    int n = 0;
    if (prec < 0) prec = 6;

    /* Handle sign and special values. */
    if (v != v) { out[n++] = 'n'; out[n++] = 'a'; out[n++] = 'n'; return n; }
    if (v < 0) { out[n++] = '-'; v = -v; }
    if (v > 1e308) { out[n++] = 'i'; out[n++] = 'n'; out[n++] = 'f'; return n; }

    /* Decompose into mantissa + base-10 exponent. */
    int exp = 0;
    if (v > 0) {
        while (v >= 10.0)   { v /= 10.0; exp++; }
        while (v < 1.0 && exp > -300) { v *= 10.0; exp--; }
    }

    /* For %g, pick %e or %f based on exponent. */
    int use_exp = (conv == 'e') ||
                  (conv == 'g' && (exp < -4 || exp >= prec));

    int digits = (conv == 'g') ? (prec > 0 ? prec : 1) : prec + 1;
    /* Round: add 5 * 10^(-digits) before truncating. */
    double round = 0.5;
    for (int i = 0; i < digits; i++) round /= 10.0;
    v += round;
    if (v >= 10.0) { v /= 10.0; exp++; }

    if (use_exp) {
        /* d.dddddde+/-NN */
        int d = (int)v;
        out[n++] = '0' + d;
        v -= d;
        if (digits > 1) out[n++] = '.';
        for (int i = 1; i < digits; i++) {
            v *= 10.0;
            int dd = (int)v;
            if (dd < 0) dd = 0; if (dd > 9) dd = 9;
            out[n++] = '0' + dd;
            v -= dd;
        }
        out[n++] = 'e';
        int e = exp;
        if (e < 0) { out[n++] = '-'; e = -e; } else out[n++] = '+';
        if (e < 10) out[n++] = '0';
        char ebuf[6]; int ei = 0;
        if (e == 0) ebuf[ei++] = '0';
        while (e) { ebuf[ei++] = '0' + (e % 10); e /= 10; }
        while (ei > 0) out[n++] = ebuf[--ei];
    } else {
        /* Plain decimal with `prec` or `exp+1` integer digits. */
        int int_digits = exp + 1;
        if (int_digits < 1) int_digits = 1;
        int after_dot = (conv == 'g') ? (prec - int_digits) : prec;
        if (after_dot < 0) after_dot = 0;

        /* Integer part. */
        if (exp < 0) {
            out[n++] = '0';
        } else {
            for (int i = 0; i <= exp; i++) {
                int dd = (int)v;
                if (dd < 0) dd = 0; if (dd > 9) dd = 9;
                out[n++] = '0' + dd;
                v -= dd;
                v *= 10.0;
            }
        }

        /* Fractional part. */
        if (after_dot > 0) {
            out[n++] = '.';
            if (exp < 0) {
                /* Insert leading zeros for values < 1. */
                for (int i = exp + 1; i < 0 && after_dot > 0; i++) {
                    out[n++] = '0';
                    after_dot--;
                }
            }
            for (int i = 0; i < after_dot; i++) {
                int dd = (int)v;
                if (dd < 0) dd = 0; if (dd > 9) dd = 9;
                out[n++] = '0' + dd;
                v -= dd;
                v *= 10.0;
            }
            /* For %g, trim trailing zeros. */
            if (conv == 'g') {
                while (n > 0 && out[n-1] == '0') n--;
                if (n > 0 && out[n-1] == '.') n--;
            }
        }
    }
    return n;
}

int vsnprintf(char *out, size_t size, const char *fmt, va_list ap) {
    buf_t b = { out, 0, size };
    for (; *fmt; fmt++) {
        if (*fmt != '%') { emit(&b, *fmt); continue; }
        fmt++;

        /* Flags */
        int left = 0, zero = 0, plus = 0, space = 0, alt = 0;
        for (;; fmt++) {
            if (*fmt == '-') left = 1;
            else if (*fmt == '0') zero = 1;
            else if (*fmt == '+') plus = 1;
            else if (*fmt == ' ') space = 1;
            else if (*fmt == '#') alt = 1;
            else break;
        }

        /* Width */
        int width = 0;
        if (*fmt == '*') { width = va_arg(ap, int); fmt++; }
        else while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt - '0'); fmt++; }

        /* Precision */
        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') { prec = va_arg(ap, int); fmt++; }
            else while (*fmt >= '0' && *fmt <= '9') { prec = prec * 10 + (*fmt - '0'); fmt++; }
        }

        /* Length modifier */
        int lng = 0;  /* 0 = int, 1 = long, 2 = long long, 3 = size_t */
        if (*fmt == 'l') { lng = 1; fmt++; if (*fmt == 'l') { lng = 2; fmt++; } }
        else if (*fmt == 'z') { lng = 3; fmt++; }
        else if (*fmt == 'h') { fmt++; if (*fmt == 'h') fmt++; }

        char conv = *fmt;
        if (!conv) break;

        char tmp[32];
        int tmplen = 0;
        const char *prefix = "";
        int pref_len = 0;
        int neg = 0;
        unsigned long long u = 0;

        switch (conv) {
        case 'd': case 'i': {
            long long v;
            if (lng >= 2) v = va_arg(ap, long long);
            else if (lng == 1) v = va_arg(ap, long);
            else v = va_arg(ap, int);
            if (v < 0) { neg = 1; u = (unsigned long long)(-v); }
            else u = (unsigned long long)v;
            tmplen = utoa(u, 10, 0, tmp);
            if (neg) { prefix = "-"; pref_len = 1; }
            else if (plus) { prefix = "+"; pref_len = 1; }
            else if (space) { prefix = " "; pref_len = 1; }
            break;
        }
        case 'u': {
            if (lng >= 2) u = va_arg(ap, unsigned long long);
            else if (lng == 1) u = va_arg(ap, unsigned long);
            else u = va_arg(ap, unsigned int);
            tmplen = utoa(u, 10, 0, tmp);
            break;
        }
        case 'x': case 'X': {
            if (lng >= 2) u = va_arg(ap, unsigned long long);
            else if (lng == 1) u = va_arg(ap, unsigned long);
            else u = va_arg(ap, unsigned int);
            tmplen = utoa(u, 16, conv == 'X', tmp);
            if (alt && u) {
                prefix = (conv == 'X') ? "0X" : "0x";
                pref_len = 2;
            }
            break;
        }
        case 'o': {
            if (lng >= 2) u = va_arg(ap, unsigned long long);
            else if (lng == 1) u = va_arg(ap, unsigned long);
            else u = va_arg(ap, unsigned int);
            tmplen = utoa(u, 8, 0, tmp);
            if (alt) { prefix = "0"; pref_len = 1; }
            break;
        }
        case 'p': {
            u = (unsigned long)va_arg(ap, void *);
            tmplen = utoa(u, 16, 0, tmp);
            prefix = "0x";
            pref_len = 2;
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int slen = 0;
            while (s[slen] && (prec < 0 || slen < prec)) slen++;
            int pad = width - slen;
            if (!left) while (pad-- > 0) emit(&b, ' ');
            for (int i = 0; i < slen; i++) emit(&b, s[i]);
            if (left) while (pad-- > 0) emit(&b, ' ');
            continue;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            int pad = width - 1;
            if (!left) while (pad-- > 0) emit(&b, ' ');
            emit(&b, c);
            if (left) while (pad-- > 0) emit(&b, ' ');
            continue;
        }
        case 'g': case 'G': case 'e': case 'E': case 'f': case 'F': {
            double v = va_arg(ap, double);
            char fbuf[64];
            char which = (conv == 'G' || conv == 'E' || conv == 'F') ? conv + 32 : conv;
            int flen = dtoa(v, which, (prec >= 0) ? prec : 6, fbuf);
            int pad = width - flen;
            if (!left) while (pad-- > 0) emit(&b, ' ');
            for (int i = 0; i < flen; i++) emit(&b, fbuf[i]);
            if (left) while (pad-- > 0) emit(&b, ' ');
            continue;
        }
        case '%': emit(&b, '%'); continue;
        default:
            emit(&b, '%');
            emit(&b, conv);
            continue;
        }

        /* Numeric formatting: prefix + zero/space pad + digits */
        int num_len = pref_len + tmplen;
        int zeros = 0;
        if (prec >= 0) {
            if (prec > tmplen) zeros = prec - tmplen;
            if (prec == 0 && u == 0) tmplen = 0;  /* %.0d of 0 -> "" */
            num_len = pref_len + tmplen + zeros;
            zero = 0;  /* precision overrides zero-pad */
        }
        int pad = width - num_len;
        if (!left && !zero) while (pad-- > 0) emit(&b, ' ');
        emit_str(&b, prefix, pref_len);
        if (!left && zero) while (pad-- > 0) emit(&b, '0');
        while (zeros-- > 0) emit(&b, '0');
        while (tmplen-- > 0) emit(&b, tmp[tmplen]);
        if (left) while (pad-- > 0) emit(&b, ' ');
    }

    if (b.cap > 0) {
        b.out[(b.pos < b.cap) ? b.pos : b.cap - 1] = '\0';
    }
    return (int)b.pos;
}

int snprintf(char *out, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out, size, fmt, ap);
    va_end(ap);
    return n;
}

int vsprintf(char *out, const char *fmt, va_list ap) {
    /* Unsafe — caller promises the buffer is big enough. Pass a huge
       cap so vsnprintf never truncates. */
    return vsnprintf(out, (size_t)-1 / 2, fmt, ap);
}

int sprintf(char *out, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsprintf(out, fmt, ap);
    va_end(ap);
    return n;
}
