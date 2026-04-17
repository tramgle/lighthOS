/* tail [-n N] [file] — last N lines (default 10). Simple ring. */
#include "ulib_x64.h"

#define TAIL_MAX_N 64
#define TAIL_LINE_MAX 512

int main(int argc, char **argv, char **envp) {
    (void)envp;
    int n = 10;
    int arg = 1;
    if (arg < argc && u_strcmp(argv[arg], "-n") == 0 && arg + 1 < argc) {
        n = u_atoi(argv[arg + 1]);
        if (n < 0) n = 0;
        if (n > TAIL_MAX_N) n = TAIL_MAX_N;
        arg += 2;
    }
    int fd = (arg < argc) ? sys_open(argv[arg], O_RDONLY) : 0;
    if (fd < 0) return 1;

    static char ring[TAIL_MAX_N][TAIL_LINE_MAX];
    int head_i = 0, count = 0;
    char line[TAIL_LINE_MAX]; int llen = 0;
    char c;
    while (sys_read(fd, &c, 1) == 1) {
        if (llen < TAIL_LINE_MAX - 1) line[llen++] = c;
        if (c == '\n') {
            line[llen] = 0;
            u_memcpy(ring[head_i], line, (size_t)llen + 1);
            head_i = (head_i + 1) % TAIL_MAX_N;
            if (count < TAIL_MAX_N) count++;
            llen = 0;
        }
    }
    if (llen > 0) {
        line[llen] = 0;
        u_memcpy(ring[head_i], line, (size_t)llen + 1);
        head_i = (head_i + 1) % TAIL_MAX_N;
        if (count < TAIL_MAX_N) count++;
    }
    if (fd > 0) sys_close(fd);

    int emit = n < count ? n : count;
    int start = (head_i - emit + TAIL_MAX_N) % TAIL_MAX_N;
    for (int i = 0; i < emit; i++) {
        const char *s = ring[(start + i) % TAIL_MAX_N];
        u_puts_n(s);
    }
    return 0;
}
