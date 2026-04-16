#ifndef _SIGNAL_H
#define _SIGNAL_H

/* Thin shim over the real ulib signal() so Lua (which installs a SIGINT
   handler) can compile with the vendored libc headers. sighandler_t,
   SIG_DFL/IGN/ERR, and signal() itself live in ulib.h; we only add the
   POSIX-style SIGINT/SIGTERM/SIGABRT names Lua expects. */

#include "ulib.h"

typedef int sig_atomic_t;

#define SIGINT   2
#define SIGTERM 15
#define SIGABRT  6

static inline int raise(int sig) { (void)sig; return 0; }

#endif
