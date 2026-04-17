/* test_fs: write + read + unlink round-trip in the current root fs. */
#include "syscall_x64.h"

static int bytes_equal(const char *a, const char *b, long n) {
    for (long i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

int main(int argc, char **argv, char **envp) {
    (void)argc; (void)argv; (void)envp;

    const char *msg = "round-trip ok\n";
    long mlen = 0; while (msg[mlen]) mlen++;

    int fd = sys_open("/scratch_fs.txt", O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return 30;
    if (sys_write(fd, msg, mlen) != mlen) return 31;
    sys_close(fd);

    struct vfs_stat st;
    if (sys_stat("/scratch_fs.txt", &st) != 0) return 32;
    if (st.size != (uint32_t)mlen) return 33;

    fd = sys_open("/scratch_fs.txt", O_RDONLY);
    if (fd < 0) return 34;
    char buf[64];
    long n = sys_read(fd, buf, sizeof(buf));
    sys_close(fd);
    if (n != mlen) return 35;
    if (!bytes_equal(buf, msg, mlen)) return 36;

    if (sys_unlink("/scratch_fs.txt") != 0) return 37;
    if (sys_stat("/scratch_fs.txt", &st) == 0) return 38;

    return 0;
}
