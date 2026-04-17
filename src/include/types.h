#ifndef TYPES_H
#define TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>

/* Kernel-side typedefs. Widened to 64-bit on x86_64 so size_t/off_t
   arithmetic with 64-bit pointers and large (>4 GiB) offsets works
   without truncation. User-space has its own ssize_t/off_t in
   user/syscall_x64.h; the two namespaces don't mix. */
typedef int64_t  ssize_t;
typedef int64_t  off_t;
typedef uint64_t mode_t;
typedef uint64_t ino_t;

#endif
