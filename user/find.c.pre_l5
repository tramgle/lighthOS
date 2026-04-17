/* find <dir>: recursive listing. Prints each path under the start
   directory, one per line. No -name / -type filters yet — pipe to
   grep if you need filtering. */

#include "syscall.h"
#include "ulib.h"

#define FIND_MAX_DEPTH 32

static int path_append(char *path, int *len, int cap, const char *name) {
    int nlen = 0;
    while (name[nlen]) nlen++;
    int need_sep = (*len > 0 && path[*len - 1] != '/') ? 1 : 0;
    if (*len + need_sep + nlen + 1 > cap) return -1;
    if (need_sep) path[(*len)++] = '/';
    for (int i = 0; i < nlen; i++) path[(*len)++] = name[i];
    path[*len] = '\0';
    return 0;
}

static void walk(char *path, int *len, int cap, int depth) {
    sys_write(1, path, *len);
    sys_write(1, "\n", 1);

    struct vfs_stat st;
    if (sys_stat(path, &st) != 0 || st.type != VFS_DIR) return;
    if (depth >= FIND_MAX_DEPTH) return;

    char name[VFS_MAX_NAME];
    uint32_t type;
    for (uint32_t idx = 0; sys_readdir(path, idx, name, &type) == 0; idx++) {
        if (name[0] == '.' &&
            (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) continue;
        int saved = *len;
        if (path_append(path, len, cap, name) != 0) continue;
        walk(path, len, cap, depth + 1);
        *len = saved;
        path[*len] = '\0';
    }
}

int main(int argc, char **argv) {
    char path[VFS_MAX_PATH * 2];
    int len;

    const char *start = (argc >= 2) ? argv[1] : ".";
    len = 0;
    while (start[len] && len < (int)sizeof(path) - 1) {
        path[len] = start[len];
        len++;
    }
    path[len] = '\0';

    walk(path, &len, sizeof path, 0);
    return 0;
}
