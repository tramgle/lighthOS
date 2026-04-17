#include "kernel/ksyms.h"

/* Weak stubs live in ksyms_stub.c so this translation unit sees
 * ksyms[] / ksym_count / ksym_strs only through the extern
 * declarations in the header — that forces every reference to go
 * through a real load instead of being constant-folded from a
 * compile-time-visible initialiser. */
const char *ksym_lookup(uint64_t rip, uint64_t *offset_out) {
    if (ksym_count == 0 || rip < ksyms[0].addr) return 0;

    uint32_t lo = 0, hi = ksym_count;
    while (lo + 1 < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (ksyms[mid].addr <= rip) lo = mid;
        else                         hi = mid;
    }
    if (offset_out) *offset_out = rip - ksyms[lo].addr;
    return &ksym_strs[ksyms[lo].name_off];
}
