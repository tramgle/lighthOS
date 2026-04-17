#include "kernel/ksyms.h"

/* Weak stubs so stage1 of the kernel link (the one without
 * build/ksyms_data.o) resolves these three data symbols. Stage2
 * relinks with the generator's strong definitions, which override
 * the weak ones.
 *
 * These live in their own translation unit so ksym_lookup in
 * ksyms.c can't see the initialiser at compile time and
 * constant-fold an array element to zero — it must load the
 * linker-resolved value at runtime. */
__attribute__((weak)) const struct ksym ksyms[1]     = {{0, 0}};
__attribute__((weak)) const uint32_t    ksym_count   = 0;
__attribute__((weak)) const char        ksym_strs[1] = "";
