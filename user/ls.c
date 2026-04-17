/* ls [-lCa1] [path...]: list directory contents.
 *
 *   -l  long form: type + size + name, one per line
 *   -a  include dotfiles
 *   -C  force multi-column grid even when not a tty
 *   -1  force one-per-line even when stdout is a tty
 *
 * Default: sort alphabetically, grid when stdout is the console,
 * one-per-line otherwise (pipes / redirects). Real binary so it
 * composes in pipes: `ls /bin | grep foo | wc -l`.
 */

#include "ulib_x64.h"

#define MAX_ENTRIES 256
#define MAX_NAME     64

struct readdir_out {
    char     name[MAX_NAME];
    uint32_t type;
};

struct entry {
    char     name[MAX_NAME];
    uint32_t type;
    uint32_t size;    /* -l only; left 0 otherwise */
};

static struct entry entries[MAX_ENTRIES];

static void print_padded_uint(uint32_t v, int width) {
    char buf[16]; int n = 0;
    if (v == 0) buf[n++] = '0';
    while (v > 0) { buf[n++] = '0' + (v % 10); v /= 10; }
    for (int i = n; i < width; i++) u_putc(' ');
    while (n > 0) u_putc(buf[--n]);
}

static int path_join(char *out, int cap, const char *dir, const char *name) {
    int oi = 0;
    while (*dir && oi < cap - 1) out[oi++] = *dir++;
    if (oi == 0 || out[oi - 1] != '/') {
        if (oi >= cap - 1) return -1;
        out[oi++] = '/';
    }
    while (*name && oi < cap - 1) out[oi++] = *name++;
    if (oi >= cap) return -1;
    out[oi] = 0;
    return 0;
}

static int name_cmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

/* Insertion sort. N <= MAX_ENTRIES (256) → O(N^2) is fine and we
   avoid dragging qsort into ulib. Stable across equal keys, which
   matters nowhere today but costs nothing. */
static void sort_entries(int n) {
    for (int i = 1; i < n; i++) {
        struct entry key = entries[i];
        int j = i - 1;
        while (j >= 0 && name_cmp(entries[j].name, key.name) > 0) {
            entries[j + 1] = entries[j];
            j--;
        }
        entries[j + 1] = key;
    }
}

static int display_width(const struct entry *e) {
    int len = (int)u_strlen(e->name);
    if (e->type == VFS_DIR) len++;    /* trailing '/' */
    return len;
}

static void print_name(const struct entry *e) {
    u_puts_n(e->name);
    if (e->type == VFS_DIR) u_putc('/');
}

static void print_long(int n) {
    for (int i = 0; i < n; i++) {
        u_putc(entries[i].type == VFS_DIR ? 'd' : 'f');
        u_putc(' '); u_putc(' ');
        print_padded_uint(entries[i].size, 8);
        u_putc(' '); u_putc(' ');
        print_name(&entries[i]);
        u_putc('\n');
    }
}

static void print_single(int n) {
    for (int i = 0; i < n; i++) {
        print_name(&entries[i]);
        u_putc('\n');
    }
}

/* Multi-column "down-then-across" layout, matching coreutils ls -C:
 * entries fill column 0 top to bottom, then column 1, etc. Final
 * column doesn't get trailing spaces so `ls -C | cat -A` doesn't
 * show a ragged tail. */
static void print_columns(int n, int term_cols) {
    int max_w = 1;
    for (int i = 0; i < n; i++) {
        int w = display_width(&entries[i]);
        if (w > max_w) max_w = w;
    }
    int col_w = max_w + 2;
    int cols = term_cols / col_w;
    if (cols < 1) cols = 1;
    if (cols > n) cols = n;
    int rows = (n + cols - 1) / cols;

    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int i = c * rows + r;
            if (i >= n) continue;
            print_name(&entries[i]);
            if (c + 1 < cols) {
                int pad = col_w - display_width(&entries[i]);
                while (pad-- > 0) u_putc(' ');
            }
        }
        u_putc('\n');
    }
}

static int list_one(const char *path, int want_long, int want_all,
                    int force_single, int force_cols,
                    int stdout_tty, int term_cols) {
    struct vfs_stat pst;
    if (sys_stat(path, &pst) != 0) {
        u_puts_n("ls: "); u_puts_n(path); u_puts_n(": not found\n");
        return 1;
    }

    /* Single-file target: render as one entry, honouring -l. */
    if (pst.type != VFS_DIR) {
        if (want_long) {
            u_putc('f'); u_putc(' '); u_putc(' ');
            print_padded_uint(pst.size, 8);
            u_putc(' '); u_putc(' ');
        }
        u_puts_n(path); u_putc('\n');
        return 0;
    }

    /* Collect. */
    int n = 0;
    struct readdir_out e;
    for (uint32_t idx = 0;
         _syscall3(SYS_READDIR, (long)(uintptr_t)path, idx, (long)(uintptr_t)&e) == 0;
         idx++) {
        if (!want_all && e.name[0] == '.') continue;
        if (n >= MAX_ENTRIES) break;
        int k = 0;
        while (e.name[k] && k < MAX_NAME - 1) { entries[n].name[k] = e.name[k]; k++; }
        entries[n].name[k] = 0;
        entries[n].type = e.type;
        entries[n].size = 0;
        if (want_long) {
            char full[256];
            struct vfs_stat st = { 0, 0, 0 };
            if (path_join(full, sizeof full, path, entries[n].name) == 0)
                sys_stat(full, &st);
            entries[n].size = st.size;
        }
        n++;
    }

    sort_entries(n);

    int use_cols;
    if (want_long || force_single) use_cols = 0;
    else if (force_cols)           use_cols = 1;
    else                           use_cols = stdout_tty && n > 1;

    if (want_long)      print_long(n);
    else if (use_cols)  print_columns(n, term_cols);
    else                print_single(n);
    return 0;
}

int main(int argc, char **argv, char **envp) {
    (void)envp;
    int want_long = 0, want_all = 0, force_single = 0, force_cols = 0;
    int path_count = 0;
    int paths[16];

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1]) {
            for (const char *f = argv[i] + 1; *f; f++) {
                if      (*f == 'l') want_long = 1;
                else if (*f == 'a') want_all = 1;
                else if (*f == 'C') force_cols = 1;
                else if (*f == '1') force_single = 1;
                else {
                    u_puts_n("ls: unknown flag -");
                    u_putc(*f); u_putc('\n');
                    return 1;
                }
            }
        } else if (path_count < 16) {
            paths[path_count++] = i;
        }
    }

    /* Column layout needs the terminal width + confirmation that
       stdout is actually a console (sys_tty_getsize reports the
       kernel's global cached winsize regardless of where fd 1
       points — so it can't speak for pipes or file redirects by
       itself). */
    int stdout_tty = (int)sys_isatty(1);
    uint16_t rows = 0, cols = 0;
    sys_tty_getsize(&rows, &cols);
    int term_cols = cols ? (int)cols : 80;

    if (path_count == 0)
        return list_one(".", want_long, want_all, force_single, force_cols,
                        stdout_tty, term_cols);

    int rc = 0;
    for (int i = 0; i < path_count; i++) {
        if (path_count > 1) {
            u_puts_n(argv[paths[i]]); u_puts_n(":\n");
        }
        if (list_one(argv[paths[i]], want_long, want_all, force_single,
                     force_cols, stdout_tty, term_cols) != 0) rc = 1;
        if (i + 1 < path_count) u_putc('\n');
    }
    return rc;
}
