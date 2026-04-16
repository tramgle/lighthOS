/* Miscellaneous libc bits that don't fit elsewhere: process control,
   time, math stubs, noop stand-ins. */

#include "ulib.h"
#include "syscall.h"
#include <time.h>
#include <string.h>
#include <math.h>

void *malloc(uint32_t);
int   fprintf(void *f, const char *fmt, ...);
extern void *stderr;

void exit(int status) {
    sys_exit(status);
    for (;;) { }  /* unreachable */
}

void abort(void) {
    sys_write(2, "abort\n", 6);
    sys_exit(128);
    for (;;) { }
}

int atexit(void (*fn)(void)) { (void)fn; return 0; }

/* getenv is now provided by ulib — it walks the real envp passed
   in at process start. The old always-NULL stub lived here; libvibc
   picks up ulib's symbol at final-link time. */

int system(const char *cmd) { (void)cmd; return -1; }

void __libc_assert_fail(const char *expr, const char *file, int line) {
    fprintf(stderr, "assertion failed: %s at %s:%d\n", expr, file, line);
    sys_exit(1);
}

/* --- time --- */

uint32_t sys_time(void);

long time(long *t) {
    /* SYS_TIME returns 100 Hz ticks since boot. Translate to seconds. */
    long s = (long)(sys_time() / 100);
    if (t) *t = s;
    return s;
}

long clock(void) {
    /* CLOCKS_PER_SEC == 100, same as our tick rate. */
    return (long)sys_time();
}

double difftime(long a, long b) { return (double)(a - b); }

static struct tm _tm;

/* Very rough gmtime: days since 1970 / seconds-of-day only. Good enough
   for Lua's `os.date("*t", 0)` to not crash; don't trust the output. */
struct tm *gmtime(const long *tp) {
    long t = tp ? *tp : 0;
    long sec = t % 60; t /= 60;
    long min = t % 60; t /= 60;
    long hour = t % 24; t /= 24;
    _tm.tm_sec = (int)sec;
    _tm.tm_min = (int)min;
    _tm.tm_hour = (int)hour;
    _tm.tm_mday = 1 + (int)(t % 31);
    _tm.tm_mon = 0;
    _tm.tm_year = 70;
    _tm.tm_wday = 0;
    _tm.tm_yday = 0;
    _tm.tm_isdst = 0;
    return &_tm;
}

struct tm *localtime(const long *tp) { return gmtime(tp); }

long mktime(struct tm *tm) {
    (void)tm;
    return 0;
}

uint32_t strftime(char *out, uint32_t max, const char *fmt, const struct tm *tm) {
    (void)fmt; (void)tm;
    if (max > 0) out[0] = '\0';
    return 0;
}

/* --- math stubs (no lua float path should hit these in integer-only mode,
   but some reach anyway). Implement the ones Lua 5.4 actually references. --- */

double fabs(double x) { return x < 0 ? -x : x; }

double floor(double x) {
    long long i = (long long)x;
    if ((double)i > x) i--;
    return (double)i;
}

double ceil(double x) {
    long long i = (long long)x;
    if ((double)i < x) i++;
    return (double)i;
}

double fmod(double a, double b) {
    if (b == 0.0) return 0.0;
    double q = a / b;
    long long qi = (long long)q;
    return a - (double)qi * b;
}

double sqrt(double x) {
    if (x <= 0.0) return 0.0;
    double r = x, last = 0.0;
    for (int i = 0; i < 50 && r != last; i++) {
        last = r;
        r = 0.5 * (r + x / r);
    }
    return r;
}

double pow(double a, double b) {
    /* Only integer exponents hit us in practice; do repeated multiply. */
    long long e = (long long)b;
    double r = 1.0;
    double base = a;
    if (e < 0) { base = 1.0 / a; e = -e; }
    while (e > 0) {
        if (e & 1) r *= base;
        base *= base;
        e >>= 1;
    }
    return r;
}

double ldexp(double x, int exp) {
    double r = x;
    if (exp > 0) while (exp--) r *= 2.0;
    else while (exp++) r *= 0.5;
    return r;
}

double frexp(double x, int *exp) {
    /* Not right in general, but keeps things linking. */
    int e = 0;
    if (x != 0.0) {
        while (x >= 1.0) { x *= 0.5; e++; }
        while (x < 0.5 && x > 0.0) { x *= 2.0; e--; }
    }
    if (exp) *exp = e;
    return x;
}

double modf(double x, double *iptr) {
    double i = floor(x);
    if (iptr) *iptr = i;
    return x - i;
}

double trunc(double x) { return (x < 0) ? ceil(x) : floor(x); }

/* Trigonometric/transcendental stubs. Naive Taylor series for sin/cos/exp
   — accurate enough for small inputs; Lua programs that call math.sin in
   integer mode are rare. log uses Newton's method. These are placeholders
   to keep things linking; serious math code should avoid them. */

double sin(double x) {
    /* Range-reduce to [-PI, PI]. */
    const double PI = 3.14159265358979323846;
    while (x > PI) x -= 2.0 * PI;
    while (x < -PI) x += 2.0 * PI;
    double term = x, sum = x, x2 = x * x;
    for (int n = 1; n < 10; n++) {
        term *= -x2 / ((2 * n) * (2 * n + 1));
        sum += term;
    }
    return sum;
}

double cos(double x) {
    const double PI = 3.14159265358979323846;
    while (x > PI) x -= 2.0 * PI;
    while (x < -PI) x += 2.0 * PI;
    double term = 1.0, sum = 1.0, x2 = x * x;
    for (int n = 1; n < 10; n++) {
        term *= -x2 / ((2 * n - 1) * (2 * n));
        sum += term;
    }
    return sum;
}

double tan(double x) { double c = cos(x); return c == 0.0 ? 0.0 : sin(x) / c; }

double asin(double x) { return x; }   /* stub */
double acos(double x) { (void)x; return 0.0; }
double atan(double x) { return x; }
double atan2(double y, double x) { (void)x; return y; }
double sinh(double x) { return 0.5 * (exp(x) - exp(-x)); }
double cosh(double x) { return 0.5 * (exp(x) + exp(-x)); }
double tanh(double x) { double e = exp(2 * x); return (e - 1) / (e + 1); }

double exp(double x) {
    double term = 1.0, sum = 1.0;
    for (int n = 1; n < 30; n++) {
        term *= x / n;
        sum += term;
    }
    return sum;
}

double log(double x) {
    if (x <= 0.0) return 0.0;
    /* Newton's method on exp(y) = x. */
    double y = 0.0;
    for (int i = 0; i < 30; i++) {
        double e = exp(y);
        if (e == 0) break;
        y = y + (x - e) / e;
    }
    return y;
}

double log10(double x) { return log(x) / 2.302585092994046; }
double log2(double x) { return log(x) / 0.6931471805599453; }
double hypot(double a, double b) { return sqrt(a * a + b * b); }

/* --- unistd-ish --- */

int isatty(int fd) { return (fd <= 2) ? 1 : 0; }
int close(int fd) { return sys_close(fd); }
long lseek(int fd, long off, int whence) { return sys_lseek(fd, (int32_t)off, whence); }
int read(int fd, void *buf, uint32_t n) { return sys_read(fd, buf, n); }
int write(int fd, const void *buf, uint32_t n) { return sys_write(fd, buf, n); }
int unlink(const char *path) { return sys_unlink(path); }

/* --- qsort (simple insertion sort for small n) --- */

void qsort(void *base, uint32_t n, uint32_t size, int (*cmp)(const void *, const void *)) {
    char *a = (char *)base;
    for (uint32_t i = 1; i < n; i++) {
        for (uint32_t j = i; j > 0; j--) {
            char *lhs = a + (j - 1) * size;
            char *rhs = a + j * size;
            if (cmp(lhs, rhs) > 0) {
                for (uint32_t k = 0; k < size; k++) {
                    char t = lhs[k]; lhs[k] = rhs[k]; rhs[k] = t;
                }
            } else break;
        }
    }
}

/* --- pseudo-random --- */

static uint32_t rand_state = 1;
int rand(void) { rand_state = rand_state * 1103515245u + 12345u; return (int)((rand_state >> 16) & 0x7fffffff); }
void srand(unsigned s) { rand_state = s; }

/* strcoll alias — we have no locale-aware collation. */
int strcoll(const char *a, const char *b) { return strcmp(a, b); }

int abs(int x) { return x < 0 ? -x : x; }
long labs(long x) { return x < 0 ? -x : x; }
long long llabs(long long x) { return x < 0 ? -x : x; }

typedef struct { int quot; int rem; } div_t;
typedef struct { long quot; long rem; } ldiv_t;
div_t  div(int num, int den) { div_t r = { num / den, num % den }; return r; }
ldiv_t ldiv(long num, long den) { ldiv_t r = { num / den, num % den }; return r; }
