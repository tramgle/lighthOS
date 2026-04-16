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

/* ---- Signal handling ---------------------------------------------
   Architecture: the kernel's SYS_SIGNAL registers a raw function
   address and, on delivery, builds a [retaddr=0][signo] frame on the
   user stack and sets EIP to that address. So we hand the kernel a
   single shared trampoline (`_ulib_sig_thunk`) that grabs signo from
   4(%esp), dispatches through a per-process table, then calls
   sys_sigreturn to restore the pre-handler state. This lets user code
   register plain `void f(int)` handlers without caring about the
   trampoline convention. */

static sighandler_t _user_sighandlers[NSIG_USER];

/* Entry point the kernel jumps to on signal delivery. The kernel sets
   up the user stack as [retaddr=0][signo] so a normal cdecl-callable
   function sees its argument in the usual place. noinline+used keeps
   the body around — its address is stored via an int cast, which
   dead-code elimination would otherwise drop. */
__attribute__((noinline, used))
static void _ulib_sig_thunk(int signo) {
    if (signo >= 0 && signo < NSIG_USER && _user_sighandlers[signo]) {
        sighandler_t h = _user_sighandlers[signo];
        if (h != SIG_IGN && h != SIG_DFL) h(signo);
    }
    sys_sigreturn();  /* never returns */
}

/* ---- Environment variables ------------------------------------
   `environ` is set by crt0 from the SysV stack. Reading (getenv)
   walks it verbatim. First mutation (setenv/unsetenv) copies the
   pointer array into our own fixed-size backing slot so we can
   modify it without touching the initial stack storage — the
   original stack-resident envp is logically read-only. */

char **environ;

#define ENV_POOL_SIZE 4096
#define ENV_MAX       64

static char *env_backing[ENV_MAX];
static int   env_backing_live;
static char  env_pool[ENV_POOL_SIZE];
static int   env_pool_used;

static void env_adopt(void) {
    if (env_backing_live) return;
    int i = 0;
    if (environ) {
        for (; environ[i] && i < ENV_MAX - 1; i++) env_backing[i] = environ[i];
    }
    env_backing[i] = 0;
    environ = env_backing;
    env_backing_live = 1;
}

char *getenv(const char *name) {
    if (!name || !environ) return 0;
    size_t nlen = strlen(name);
    for (int i = 0; environ[i]; i++) {
        char *e = environ[i];
        if (strncmp(e, name, nlen) == 0 && e[nlen] == '=') {
            return e + nlen + 1;
        }
    }
    return 0;
}

int setenv(const char *name, const char *value, int overwrite) {
    if (!name || !value || !*name) return -1;
    for (const char *p = name; *p; p++) if (*p == '=') return -1;
    env_adopt();

    size_t nlen = strlen(name);
    int slot = -1;
    for (int i = 0; environ[i]; i++) {
        if (strncmp(environ[i], name, nlen) == 0 && environ[i][nlen] == '=') {
            if (!overwrite) return 0;
            slot = i; break;
        }
    }

    size_t vlen = strlen(value);
    size_t need = nlen + 1 + vlen + 1;
    if ((size_t)env_pool_used + need > sizeof env_pool) return -1;
    char *dst = env_pool + env_pool_used;
    env_pool_used += (int)need;
    memcpy(dst, name, nlen);
    dst[nlen] = '=';
    memcpy(dst + nlen + 1, value, vlen);
    dst[nlen + 1 + vlen] = '\0';

    if (slot >= 0) {
        environ[slot] = dst;
    } else {
        int n = 0;
        while (environ[n]) n++;
        if (n >= ENV_MAX - 1) return -1;
        environ[n] = dst;
        environ[n + 1] = 0;
    }
    return 0;
}

int unsetenv(const char *name) {
    if (!name || !environ) return 0;
    env_adopt();
    size_t nlen = strlen(name);
    for (int i = 0; environ[i]; i++) {
        if (strncmp(environ[i], name, nlen) == 0 && environ[i][nlen] == '=') {
            int j = i;
            while (environ[j + 1]) { environ[j] = environ[j + 1]; j++; }
            environ[j] = 0;
            return 0;
        }
    }
    return 0;
}

/* ---- dlopen / dlsym wrappers ----
   Thin forwarders into ld-vibeos.so.1's function table, published
   at DL_IFACE_ADDR by the interpreter on startup. Layout must stay
   in sync with user/ldso/ld_main.c — if either changes, both move. */

#define DL_IFACE_ADDR 0x50000000u

struct _dl_ops {
    void *(*dlopen)(const char *, int);
    void *(*dlsym)(void *, const char *);
    int   (*dlclose)(void *);
    const char *(*dlerror)(void);
};

void *dlopen(const char *path, int flags) {
    return ((struct _dl_ops *)DL_IFACE_ADDR)->dlopen(path, flags);
}
void *dlsym(void *handle, const char *name) {
    return ((struct _dl_ops *)DL_IFACE_ADDR)->dlsym(handle, name);
}
int dlclose(void *handle) {
    return ((struct _dl_ops *)DL_IFACE_ADDR)->dlclose(handle);
}
const char *dlerror(void) {
    return ((struct _dl_ops *)DL_IFACE_ADDR)->dlerror();
}

sighandler_t signal(int signo, sighandler_t handler) {
    if (signo < 0 || signo >= NSIG_USER) return SIG_ERR;
    sighandler_t prev = _user_sighandlers[signo];
    _user_sighandlers[signo] = handler;

    /* Kernel-side disposition: SIG_DFL/SIG_IGN pass straight through
       (the kernel interprets 0 and 1 specially and never calls the
       trampoline for them). Anything else uses our thunk. */
    uint32_t karg;
    if (handler == SIG_DFL) karg = 0;
    else if (handler == SIG_IGN) karg = 1;
    else karg = (uint32_t)_ulib_sig_thunk;

    int32_t rc = sys_signal(signo, (void (*)(int))karg);
    if (rc < 0) {
        /* Kernel rejected (uncatchable or invalid) — roll back. */
        _user_sighandlers[signo] = prev;
        return SIG_ERR;
    }
    return prev ? prev : SIG_DFL;
}
