/* Install user binaries from the ramfs /bin into /disk/bin so they
   survive across reboots. Expects /disk to already be mounted (the
   kernel mounts the ATA disk automatically when present). */

#include "syscall.h"
#include "ulib.h"

#define CHUNK 4096

static int file_exists(const char *path) {
    unsigned char st[64];
    return sys_stat(path, st) == 0;
}

static int copy_file(const char *src, const char *dst) {
    int in = sys_open(src, O_RDONLY);
    if (in < 0) { printf("install: cannot read %s\n", src); return -1; }

    int out = sys_open(dst, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) { printf("install: cannot write %s\n", dst); sys_close(in); return -1; }

    char buf[CHUNK];
    int32_t n;
    uint32_t total = 0;
    while ((n = sys_read(in, buf, sizeof(buf))) > 0) {
        if (sys_write(out, buf, n) != n) {
            printf("install: write failed on %s\n", dst);
            sys_close(in); sys_close(out);
            return -1;
        }
        total += n;
    }
    sys_close(in);
    sys_close(out);
    printf("  %s -> %s (%u bytes)\n", src, dst, total);
    return 0;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    if (!file_exists("/disk")) {
        puts("install: /disk not mounted — boot with a disk (make run-disk)\n");
        return 1;
    }

    /* Ensure /disk/bin exists. */
    if (!file_exists("/disk/bin")) {
        if (sys_mkdir("/disk/bin") != 0) {
            puts("install: mkdir /disk/bin failed\n");
            return 1;
        }
    }

    puts("Installing /bin/* -> /disk/bin/*\n");

    char name[64];
    uint32_t type;
    uint32_t copied = 0, failed = 0;
    for (uint32_t i = 0; sys_readdir("/bin", i, name, &type) == 0; i++) {
        if (type != 1 /* VFS_FILE */) continue;

        char src[128];
        char dst[128];
        int p = 0;
        for (const char *s = "/bin/"; *s; s++) src[p++] = *s;
        for (int j = 0; name[j] && p < 126; j++) src[p++] = name[j];
        src[p] = '\0';

        p = 0;
        for (const char *s = "/disk/bin/"; *s; s++) dst[p++] = *s;
        for (int j = 0; name[j] && p < 126; j++) dst[p++] = name[j];
        dst[p] = '\0';

        if (copy_file(src, dst) == 0) copied++;
        else failed++;
    }

    printf("Installed %u files, %u failures\n", copied, failed);
    return failed ? 1 : 0;
}
