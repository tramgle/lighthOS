/* Install user binaries from the ramfs /bin into /disk/bin so they
   survive across reboots. Dev-flow tool: assumes a ramfs boot (ISO)
   with a FAT disk mounted at /disk. The primary install path is
   `make docker-disk` on the host; this binary is the in-QEMU
   fallback. */

#include <stdio.h>
#include "ulib_x64.h"

#define CHUNK 4096

struct readdir_ent { char name[64]; uint32_t type; };

static int file_exists(const char *path) {
    struct vfs_stat st;
    return sys_stat(path, &st) == 0;
}

static int copy_file(const char *src, const char *dst) {
    int in = sys_open(src, O_RDONLY);
    if (in < 0) { printf("install: cannot read %s\n", src); return -1; }
    int out = sys_open(dst, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) { printf("install: cannot write %s\n", dst); sys_close(in); return -1; }

    char buf[CHUNK];
    long n;
    uint32_t total = 0;
    while ((n = sys_read(in, buf, sizeof(buf))) > 0) {
        if (sys_write(out, buf, n) != n) {
            printf("install: write failed on %s\n", dst);
            sys_close(in); sys_close(out);
            return -1;
        }
        total += (uint32_t)n;
    }
    sys_close(in);
    sys_close(out);
    printf("  %s -> %s (%u bytes)\n", src, dst, total);
    return 0;
}

static void join(char *out, const char *pre, const char *name) {
    int p = 0;
    for (const char *s = pre; *s && p < 126; s++) out[p++] = *s;
    for (int j = 0; name[j] && p < 126; j++) out[p++] = name[j];
    out[p] = 0;
}

static uint32_t install_dir(const char *src_dir, const char *dst_dir) {
    if (!file_exists(dst_dir)) sys_mkdir(dst_dir);

    struct readdir_ent e;
    uint32_t copied = 0;
    for (uint32_t i = 0;
         _syscall3(SYS_READDIR, (long)(uintptr_t)src_dir, i,
                   (long)(uintptr_t)&e) == 0;
         i++) {
        if (e.type != VFS_FILE) continue;
        char src[128], dst[128];
        char sprefix[64], dprefix[64];
        int sp = 0; for (const char *s = src_dir; *s; s++) sprefix[sp++] = *s;
        if (sp == 0 || sprefix[sp - 1] != '/') sprefix[sp++] = '/';
        sprefix[sp] = 0;
        int dp = 0; for (const char *s = dst_dir; *s; s++) dprefix[dp++] = *s;
        if (dp == 0 || dprefix[dp - 1] != '/') dprefix[dp++] = '/';
        dprefix[dp] = 0;
        join(src, sprefix, e.name);
        join(dst, dprefix, e.name);
        if (copy_file(src, dst) == 0) copied++;
    }
    return copied;
}

int main(int argc, char **argv, char **envp) {
    (void)argc; (void)argv; (void)envp;

    if (!file_exists("/disk")) {
        puts("install: /disk not mounted — boot with a disk (make run-disk)");
        return 1;
    }

    puts("Installing /bin/* -> /disk/bin/*");
    uint32_t bcount = install_dir("/bin", "/disk/bin");
    printf("  %u binaries installed\n", bcount);

    if (file_exists("/lib")) {
        puts("Installing /lib/* -> /disk/lib/*");
        uint32_t lcount = install_dir("/lib", "/disk/lib");
        printf("  %u runtime files installed\n", lcount);
    }

    puts("Install complete. Reboot from the disk to run it.");
    return 0;
}
