/* wc [-l|-w|-c] [file...]: count lines, words, bytes. No flag: print
   all three. Reads stdin when no file argument. */

#include "syscall.h"
#include "ulib.h"

typedef struct { uint32_t lines, words, bytes; } wc_t;

static void wc_fd(int fd, wc_t *o) {
    char buf[1024];
    int32_t n;
    int in_word = 0;
    while ((n = sys_read(fd, buf, sizeof buf)) > 0) {
        o->bytes += (uint32_t)n;
        for (int i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n') o->lines++;
            int is_space = (c == ' ' || c == '\t' || c == '\n' ||
                            c == '\r' || c == '\v' || c == '\f');
            if (is_space) {
                in_word = 0;
            } else if (!in_word) {
                in_word = 1;
                o->words++;
            }
        }
    }
}

static void print_counts(wc_t *o, const char *label, int show_l, int show_w, int show_c) {
    int first = 1;
    if (show_l) { printf("%s%u", first ? "" : " ", o->lines); first = 0; }
    if (show_w) { printf("%s%u", first ? "" : " ", o->words); first = 0; }
    if (show_c) { printf("%s%u", first ? "" : " ", o->bytes); first = 0; }
    if (label) printf(" %s", label);
    sys_write(1, "\n", 1);
}

int main(int argc, char **argv) {
    int show_l = 0, show_w = 0, show_c = 0;
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        for (const char *f = argv[i] + 1; *f; f++) {
            if (*f == 'l') show_l = 1;
            else if (*f == 'w') show_w = 1;
            else if (*f == 'c') show_c = 1;
            else { printf("wc: unknown flag -%c\n", *f); return 1; }
        }
    }
    if (!show_l && !show_w && !show_c) show_l = show_w = show_c = 1;

    wc_t total = {0, 0, 0};
    int file_count = argc - i;

    if (file_count == 0) {
        wc_t o = {0, 0, 0};
        wc_fd(0, &o);
        print_counts(&o, 0, show_l, show_w, show_c);
    } else {
        for (; i < argc; i++) {
            int fd = sys_open(argv[i], O_RDONLY);
            if (fd < 0) { printf("wc: %s: not found\n", argv[i]); continue; }
            wc_t o = {0, 0, 0};
            wc_fd(fd, &o);
            sys_close(fd);
            print_counts(&o, argv[i], show_l, show_w, show_c);
            total.lines += o.lines;
            total.words += o.words;
            total.bytes += o.bytes;
        }
        if (file_count > 1) print_counts(&total, "total", show_l, show_w, show_c);
    }
    return 0;
}
