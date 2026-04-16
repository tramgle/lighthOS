/* sleep N — pause for N seconds (or N.S fractional, to two decimals).
   Implemented as a poll on SYS_TIME because the kernel has no nanosleep
   or blocking timer syscall yet. Ticks are at 100Hz. */

#include "syscall.h"
#include "ulib.h"

/* Atoi-with-fractional-part. Returns duration in 1/100-second ticks.
   "2" -> 200, "2.5" -> 250, "0.1" -> 10. Non-numeric bytes end parse. */
static uint32_t parse_ticks(const char *s) {
    uint32_t whole = 0;
    int have_int = 0;
    while (*s >= '0' && *s <= '9') { whole = whole * 10 + (*s - '0'); s++; have_int = 1; }
    uint32_t frac = 0;
    if (*s == '.') {
        s++;
        int digits = 0;
        while (*s >= '0' && *s <= '9' && digits < 2) {
            frac = frac * 10 + (*s - '0');
            s++; digits++;
        }
        if (digits == 1) frac *= 10;  /* ".5" -> 50 hundredths */
        while (*s >= '0' && *s <= '9') s++;  /* swallow excess */
    }
    if (!have_int && frac == 0) return 0;
    return whole * 100 + frac;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        puts("usage: sleep SECONDS\n");
        return 1;
    }
    uint32_t ticks = parse_ticks(argv[1]);
    uint32_t start = sys_time();
    while (sys_time() - start < ticks) {
        sys_yield();
    }
    return 0;
}
