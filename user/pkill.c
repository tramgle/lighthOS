/* pkill — signal processes by pid or name.
 *
 *   pkill <target>          — SIGTERM the matches
 *   pkill -<signo> <target> — send a specific signal number
 *   pkill -KILL <target>    — same with a short name
 *   pkill -l                — list signal names
 *   pkill -n <name>         — force name match (skips pid heuristic)
 *
 * <target> is treated as a PID when every character is a digit,
 * otherwise as a substring that must appear anywhere in the
 * process's name. -n forces name mode even for all-digit names. */

#include "ulib_x64.h"

struct sigmap { const char *name; int num; };
static const struct sigmap sigs[] = {
    { "HUP",   SIG_HUP  }, { "INT",  SIG_INT  },
    { "KILL",  SIG_KILL }, { "ALRM", SIG_ALRM },
    { "TERM",  SIG_TERM }, { "CONT", SIG_CONT },
    { "STOP",  SIG_STOP },
};

static int is_all_digits(const char *s) {
    if (!s || !*s) return 0;
    for (int i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
    }
    return 1;
}

static int contains(const char *hay, const char *needle) {
    for (size_t i = 0; hay[i]; i++) {
        size_t j = 0;
        while (needle[j] && hay[i + j] == needle[j]) j++;
        if (!needle[j]) return 1;
    }
    return 0;
}

static int parse_signal(const char *arg) {
    /* Accept -N (numeric) or -NAME (symbolic) after the leading '-'. */
    if (!arg || arg[0] != '-') return -1;
    const char *p = arg + 1;
    if (is_all_digits(p)) return u_atoi(p);
    /* "SIG" prefix tolerated: -SIGINT, -INT both work. */
    if (p[0] == 'S' && p[1] == 'I' && p[2] == 'G') p += 3;
    for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
        if (u_strcmp(p, sigs[i].name) == 0) return sigs[i].num;
    }
    return -1;
}

static void list_signals(void) {
    for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
        u_putc(' '); u_putdec(sigs[i].num);
        u_putc(')'); u_putc(' ');
        u_puts_n(sigs[i].name); u_putc('\n');
    }
}

int main(int argc, char **argv, char **envp) {
    (void)envp;
    if (argc < 2) {
        u_puts_n("usage: pkill [-signal] [-n] <pid|name>\n"
                 "       pkill -l        # list signal names\n");
        return 1;
    }

    int signo = SIG_TERM;
    int force_name = 0;
    const char *target = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (u_strcmp(a, "-l") == 0) { list_signals(); return 0; }
        if (u_strcmp(a, "-n") == 0) { force_name = 1; continue; }
        if (a[0] == '-' && a[1]) {
            int s = parse_signal(a);
            if (s <= 0) {
                u_puts_n("pkill: unknown signal "); u_puts_n(a); u_putc('\n');
                return 2;
            }
            signo = s;
            continue;
        }
        target = a;
    }
    if (!target) { u_puts_n("pkill: missing target\n"); return 1; }

    /* PID mode: exact match on one process. */
    if (!force_name && is_all_digits(target)) {
        int pid = u_atoi(target);
        if (sys_kill(pid, signo) != 0) {
            u_puts_n("pkill: no such pid "); u_puts_n(target); u_putc('\n');
            return 1;
        }
        u_puts_n("pkill: signaled "); u_putdec(pid); u_putc('\n');
        return 0;
    }

    /* Name mode: sys_ps-scan, kill every match. Skip our own pid so
       `pkill pkill` doesn't nuke us mid-iteration. */
    int self = (int)sys_getpid();
    int hits = 0;
    struct proc_info p;
    for (uint32_t i = 0; sys_ps(i, &p) == 0; i++) {
        if ((int)p.pid == self) continue;
        if (!contains(p.name, target)) continue;
        if (sys_kill((int)p.pid, signo) == 0) {
            u_puts_n("pkill: "); u_putdec(p.pid); u_putc(' ');
            u_puts_n(p.name); u_putc('\n');
            hits++;
        }
    }
    if (hits == 0) {
        u_puts_n("pkill: no matches for "); u_puts_n(target); u_putc('\n');
        return 1;
    }
    return 0;
}
