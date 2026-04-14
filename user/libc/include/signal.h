#ifndef _SIGNAL_H
#define _SIGNAL_H

/* Signals aren't supported; provide constants and stub signal() so that
   Lua's `lua.c` (which installs SIGINT handlers) can compile. */

typedef int sig_atomic_t;

#define SIG_ERR  ((void (*)(int)) -1)
#define SIG_DFL  ((void (*)(int))  0)
#define SIG_IGN  ((void (*)(int))  1)

#define SIGINT   2
#define SIGTERM 15
#define SIGABRT  6

typedef void (*sighandler_t)(int);

static inline sighandler_t signal(int sig, sighandler_t h) {
    (void)sig; (void)h;
    return SIG_DFL;
}

static inline int raise(int sig) { (void)sig; return 0; }

#endif
