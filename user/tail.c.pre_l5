/* tail [-n N] [file]: print the last N lines (default 10). Reads
   stdin when no file argument. Uses a fixed-size ring buffer —
   N is clamped to TAIL_MAX_N. */

#include "syscall.h"
#include "ulib.h"

#define TAIL_MAX_N    64
#define TAIL_LINE_MAX 512

static char ring[TAIL_MAX_N][TAIL_LINE_MAX];
static int  ring_len[TAIL_MAX_N];

static int parse_int(const char *s) {
    int n = 0;
    while (*s >= '0' && *s <= '9') { n = n * 10 + (*s - '0'); s++; }
    return n;
}

/* Push `line` (not including any trailing newline — that we add on
   print) into the ring. Overwrites the oldest slot when full. */
static void tail_fd(int fd, int n) {
    int head = 0, count = 0;
    char cur[TAIL_LINE_MAX];
    int  cur_len = 0;

    char buf[1024];
    int32_t r;
    while ((r = sys_read(fd, buf, sizeof buf)) > 0) {
        for (int i = 0; i < r; i++) {
            char c = buf[i];
            if (c == '\n') {
                int slot = (head + count) % n;
                if (count < n) count++;
                else           head = (head + 1) % n;
                int copy = cur_len < TAIL_LINE_MAX ? cur_len : TAIL_LINE_MAX;
                memcpy(ring[slot], cur, copy);
                ring_len[slot] = copy;
                cur_len = 0;
            } else if (cur_len < TAIL_LINE_MAX) {
                cur[cur_len++] = c;
            }
        }
    }
    /* Trailing line without a newline — keep it too. */
    if (cur_len > 0) {
        int slot = (head + count) % n;
        if (count < n) count++;
        else           head = (head + 1) % n;
        int copy = cur_len < TAIL_LINE_MAX ? cur_len : TAIL_LINE_MAX;
        memcpy(ring[slot], cur, copy);
        ring_len[slot] = copy;
    }

    for (int i = 0; i < count; i++) {
        int slot = (head + i) % n;
        sys_write(1, ring[slot], ring_len[slot]);
        sys_write(1, "\n", 1);
    }
}

int main(int argc, char **argv) {
    int n = 10;
    int i = 1;
    if (i < argc && strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
        n = parse_int(argv[i + 1]);
        i += 2;
    }
    if (n < 1) n = 1;
    if (n > TAIL_MAX_N) n = TAIL_MAX_N;

    if (i >= argc) {
        tail_fd(0, n);
    } else {
        for (; i < argc; i++) {
            int fd = sys_open(argv[i], O_RDONLY);
            if (fd < 0) { printf("tail: %s: not found\n", argv[i]); continue; }
            tail_fd(fd, n);
            sys_close(fd);
        }
    }
    return 0;
}
