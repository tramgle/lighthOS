/* Naive strtod / strtol / strtoul. Good enough for Lua's tonumber().
 * Accepts optional sign, decimal digits, optional fractional part, and
 * optional exponent. Uses double arithmetic — x87 FPU is available on
 * i686 even with -mno-sse. */

#include "ulib.h"

int isdigit(int c);
int isspace(int c);
int tolower(int c);

double strtod(const char *s, char **endp) {
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;

    int sign = 1;
    if (*p == '+' || *p == '-') { if (*p == '-') sign = -1; p++; }

    const char *start = p;
    double v = 0.0;
    while (isdigit((unsigned char)*p)) { v = v * 10.0 + (*p - '0'); p++; }

    if (*p == '.') {
        p++;
        double scale = 0.1;
        while (isdigit((unsigned char)*p)) {
            v += (*p - '0') * scale;
            scale *= 0.1;
            p++;
        }
    }

    if (p == start || (p == start + (v == 0.0 && *start != '.' ? 0 : 1) && !isdigit((unsigned char)*start) && *start != '.')) {
        /* No digits consumed at all. */
        if (endp) *endp = (char *)s;
        return 0.0;
    }

    if (*p == 'e' || *p == 'E') {
        p++;
        int esign = 1;
        if (*p == '+' || *p == '-') { if (*p == '-') esign = -1; p++; }
        int exp = 0;
        while (isdigit((unsigned char)*p)) { exp = exp * 10 + (*p - '0'); p++; }
        double mult = 1.0;
        for (int i = 0; i < exp; i++) mult *= 10.0;
        if (esign < 0) v /= mult;
        else v *= mult;
    }

    if (endp) *endp = (char *)p;
    return sign * v;
}

long strtol(const char *s, char **endp, int base) {
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;

    int sign = 1;
    if (*p == '+' || *p == '-') { if (*p == '-') sign = -1; p++; }

    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        base = 16;
    } else if (base == 0 && p[0] == '0') {
        base = 8;
        p++;
    } else if (base == 0) {
        base = 10;
    }

    long v = 0;
    int consumed = 0;
    while (*p) {
        int d;
        char c = tolower((unsigned char)*p);
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d;
        p++;
        consumed = 1;
    }

    if (endp) *endp = consumed ? (char *)p : (char *)s;
    return sign * v;
}

unsigned long strtoul(const char *s, char **endp, int base) {
    /* Same as strtol but return unsigned. Accept leading minus per spec
       (wraps around) but we don't bother with edge cases. */
    return (unsigned long)strtol(s, endp, base);
}

long long strtoll(const char *s, char **endp, int base) {
    /* Use strtol's logic but accumulate in long long. */
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    int sign = 1;
    if (*p == '+' || *p == '-') { if (*p == '-') sign = -1; p++; }
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2; base = 16;
    } else if (base == 0 && p[0] == '0') {
        base = 8; p++;
    } else if (base == 0) {
        base = 10;
    }
    long long v = 0;
    int consumed = 0;
    while (*p) {
        int d;
        char c = tolower((unsigned char)*p);
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d;
        p++;
        consumed = 1;
    }
    if (endp) *endp = consumed ? (char *)p : (char *)s;
    return sign * v;
}

unsigned long long strtoull(const char *s, char **endp, int base) {
    return (unsigned long long)strtoll(s, endp, base);
}

int atoi(const char *s) { return (int)strtol(s, 0, 10); }
long atol(const char *s) { return strtol(s, 0, 10); }
double atof(const char *s) { return strtod(s, 0); }
