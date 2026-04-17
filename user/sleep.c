/* sleep N[.NN] — 100 Hz tick poll. Accepts integer or
 * fractional seconds with up to two decimal places. */
#include "ulib_x64.h"

static long parse_ticks(const char *s) {
    long whole = 0;
    while (*s >= '0' && *s <= '9') { whole = whole * 10 + (*s - '0'); s++; }
    long frac = 0, place = 10;
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9' && place > 0) {
            frac += (*s - '0') * place;
            s++; place /= 10;
        }
    }
    return whole * 100 + frac;
}

int main(int argc, char **argv, char **envp) {
    (void)envp;
    if (argc < 2) return 0;
    long ticks = parse_ticks(argv[1]);
    long deadline = sys_time() + ticks;
    while (sys_time() < deadline) sys_yield();
    return 0;
}
