/* Install user binaries from the ramfs /bin into /disk/bin so they
   survive across reboots. Dev-flow tool: assumes a ramfs boot (ISO)
   with a FAT disk mounted at /disk — you can then rebuild, boot the
   bootdisk image, and the installed system sees / as the FAT root.
   The primary install path is now `make docker-disk` on the host,
   which mcopy's directly; this binary is the fallback for in-QEMU
   installs. No more chroot — the post-install flow is just reboot. */

#include "syscall.h"
#include "ulib.h"

#define CHUNK 4096

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

    char name[VFS_MAX_NAME];
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
    if (failed) return 1;

    /* Also copy /lib/*.so.1 — the dynamic linker runtime needs it
       once the majority of /bin is dynamic. Missing on an ISO that
       doesn't ship ld.so yet, so the loop silently skips absent
       sources. */
    if (!file_exists("/disk/lib")) {
        if (sys_mkdir("/disk/lib") != 0) { /* probably already exists */ }
    }
    if (file_exists("/lib")) {
        puts("Installing /lib/* -> /disk/lib/*\n");
        char lname[VFS_MAX_NAME];
        uint32_t ltype;
        uint32_t lcopied = 0;
        for (uint32_t i = 0; sys_readdir("/lib", i, lname, &ltype) == 0; i++) {
            if (ltype != 1) continue;
            char src[128], dst[128];
            int p = 0;
            for (const char *s = "/lib/"; *s; s++) src[p++] = *s;
            for (int j = 0; lname[j] && p < 126; j++) src[p++] = lname[j];
            src[p] = '\0';
            p = 0;
            for (const char *s = "/disk/lib/"; *s; s++) dst[p++] = *s;
            for (int j = 0; lname[j] && p < 126; j++) dst[p++] = lname[j];
            dst[p] = '\0';
            if (copy_file(src, dst) == 0) lcopied++;
        }
        printf("  %u runtime files installed\n", lcopied);
    }

    puts("Install complete. Reboot from the disk to run it.\n");
    return 0;
}
