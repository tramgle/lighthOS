/* ls [-la] [path...]: list directory contents.
 *   -l  long form: type + size + name
 *   -a  include dotfiles
 * With no path, lists the current directory. Real binary so it
 * composes in pipes: `ls /bin | grep foo`. */

#include "ulib_x64.h"

struct readdir_out {
    char     name[64];
    uint32_t type;
};

static size_t my_strlen(const char *s) {
    size_t n = 0; while (s[n]) n++; return n;
}

static void print_uint(uint32_t v) { u_putdec((long)v); }
static void print_padded_uint(uint32_t v, int width) {
    char buf[16]; int n = 0;
    if (v == 0) buf[n++] = '0';
    while (v > 0) { buf[n++] = '0' + (v % 10); v /= 10; }
    for (int i = n; i < width; i++) u_putc(' ');
    while (n > 0) u_putc(buf[--n]);
}

/* Join `dir` and `name` into `out`, inserting a single '/' between
   them. Returns 0 on success, -1 on overflow. */
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

static int list_one(const char *path, int want_long, int want_all) {
    struct vfs_stat pst;
    if (sys_stat(path, &pst) != 0) {
        u_puts_n("ls: "); u_puts_n(path); u_puts_n(": not found\n");
        return 1;
    }
    if (pst.type != VFS_DIR) {
        /* Single file: emulate long-form or just print the name. */
        if (want_long) {
            u_putc('f'); u_putc(' '); u_putc(' ');
            print_padded_uint(pst.size, 8);
            u_putc(' '); u_putc(' ');
        }
        u_puts_n(path); u_putc('\n');
        return 0;
    }

    struct readdir_out e;
    uint32_t shown = 0;
    for (uint32_t idx = 0;
         _syscall3(SYS_READDIR, (long)(uintptr_t)path, idx, (long)(uintptr_t)&e) == 0;
         idx++) {
        if (!want_all && e.name[0] == '.') continue;

        if (want_long) {
            char full[256];
            struct vfs_stat st = { 0, 0, 0 };
            if (path_join(full, sizeof full, path, e.name) == 0) sys_stat(full, &st);
            u_putc(e.type == VFS_DIR ? 'd' : 'f');
            u_putc(' '); u_putc(' ');
            print_padded_uint(st.size, 8);
            u_putc(' '); u_putc(' ');
            u_puts_n(e.name);
            if (e.type == VFS_DIR) u_putc('/');
            u_putc('\n');
        } else {
            u_puts_n(e.name);
            if (e.type == VFS_DIR) u_putc('/');
            u_putc('\n');
        }
        shown++;
    }
    (void)shown;
    return 0;
}

int main(int argc, char **argv, char **envp) {
    (void)envp;
    int want_long = 0, want_all = 0;
    int path_count = 0;
    int paths[16];

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1]) {
            for (const char *f = argv[i] + 1; *f; f++) {
                if (*f == 'l') want_long = 1;
                else if (*f == 'a') want_all = 1;
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

    if (path_count == 0) return list_one("/", want_long, want_all);

    int rc = 0;
    for (int i = 0; i < path_count; i++) {
        if (path_count > 1) {
            u_puts_n(argv[paths[i]]); u_puts_n(":\n");
        }
        if (list_one(argv[paths[i]], want_long, want_all) != 0) rc = 1;
        if (i + 1 < path_count) u_putc('\n');
    }
    (void)print_uint;
    (void)my_strlen;
    return rc;
}
