/* assert <name> <expected> <file> — read file, strip trailing
 * whitespace/newlines, compare to expected. Print PASS/FAIL.
 * Exit 0 if PASS, 1 on FAIL. Mirrors the pre-port contract so
 * tests/*.vsh assertions work unchanged. */
#include "ulib_x64.h"

int main(int argc, char **argv, char **envp) {
    (void)envp;
    if (argc < 4) { u_puts_n("assert: usage: name expected file\n"); return 2; }
    const char *name = argv[1];
    const char *expected = argv[2];
    const char *file = argv[3];

    char buf[1024];
    long n = u_slurp(file, buf, sizeof(buf) - 1);
    if (n < 0) {
        u_puts_n("FAIL "); u_puts_n(name);
        u_puts_n(" (no file: "); u_puts_n(file); u_puts_n(")\n");
        return 1;
    }
    buf[n] = 0;
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r' ||
                     buf[n-1] == ' '  || buf[n-1] == '\t')) {
        buf[--n] = 0;
    }

    size_t elen = u_strlen(expected);
    if ((size_t)n == elen && u_strncmp(buf, expected, elen) == 0) {
        u_puts_n("PASS "); u_puts_n(name); u_putc('\n');
        return 0;
    }
    u_puts_n("FAIL "); u_puts_n(name);
    u_puts_n(" expected='"); u_puts_n(expected);
    u_puts_n("' got='");    u_puts_n(buf); u_puts_n("'\n");
    return 1;
}
