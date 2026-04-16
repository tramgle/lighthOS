/* ls [-la] [path...]: list directory contents. -l shows type+size,
   -a includes dotfiles. With no path, lists cwd. Real binary (not a
   shell builtin) so it composes in pipes: `ls /bin | grep foo`. */

#include "syscall.h"
#include "ulib.h"

static void join(char *out, int cap, const char *dir, const char *name) {
    int oi = 0;
    while (*dir && oi < cap - 1) out[oi++] = *dir++;
    if (oi == 0 || out[oi - 1] != '/') {
        if (oi < cap - 1) out[oi++] = '/';
    }
    while (*name && oi < cap - 1) out[oi++] = *name++;
    out[oi] = '\0';
}

static int list_one(const char *path, int want_long, int want_all) {
    struct vfs_stat pst;
    if (sys_stat(path, &pst) != 0) {
        printf("ls: %s: not found\n", path);
        return 1;
    }
    if (pst.type != VFS_DIR) {
        printf("  %s\n", path);
        return 0;
    }

    char name[VFS_MAX_NAME];
    uint32_t type;
    uint32_t shown = 0;
    for (uint32_t idx = 0; sys_readdir(path, idx, name, &type) == 0; idx++) {
        if (!want_all && name[0] == '.') continue;

        if (want_long) {
            char full[VFS_MAX_PATH];
            join(full, sizeof full, path, name);
            struct vfs_stat st = { 0, 0, 0 };
            sys_stat(full, &st);
            char tch = (type == VFS_DIR) ? 'd' : 'f';
            printf("%c  %8u  %s%s\n", tch, st.size, name,
                   (type == VFS_DIR) ? "/" : "");
        } else {
            if (type == VFS_DIR) printf("  %s/\n", name);
            else                 printf("  %s\n", name);
        }
        shown++;
    }
    if (shown == 0) puts("  (empty)\n");
    return 0;
}

int main(int argc, char **argv) {
    int want_long = 0, want_all = 0;
    int path_count = 0;
    int paths[16];  /* indices into argv */

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1]) {
            for (const char *f = argv[i] + 1; *f; f++) {
                if (*f == 'l') want_long = 1;
                else if (*f == 'a') want_all = 1;
                else { printf("ls: unknown flag -%c\n", *f); return 1; }
            }
        } else if (path_count < 16) {
            paths[path_count++] = i;
        }
    }

    if (path_count == 0) {
        return list_one(".", want_long, want_all);
    }

    int rc = 0;
    for (int i = 0; i < path_count; i++) {
        if (path_count > 1) printf("%s:\n", argv[paths[i]]);
        if (list_one(argv[paths[i]], want_long, want_all) != 0) rc = 1;
        if (i + 1 < path_count) putchar('\n');
    }
    return rc;
}
