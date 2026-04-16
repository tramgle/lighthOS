/* File system round-trip test.
 *   1. stat /greeting.txt, print its size
 *   2. open + read, print contents
 *   3. write a new file, read it back
 *   4. unlink it, re-stat expecting failure
 */

#include "syscall_x64.h"

static void put_dec(long v) {
    char b[24]; int i = 0;
    if (v == 0) { uputs("0"); return; }
    if (v < 0) { uputs("-"); v = -v; }
    while (v > 0) { b[i++] = '0' + (v % 10); v /= 10; }
    while (i > 0) { char c = b[--i]; sys_write(1, &c, 1); }
}

int main(int argc, char **argv, char **envp) {
    (void)argc; (void)argv; (void)envp;

    struct vfs_stat st;
    if (sys_stat("/greeting.txt", &st) == 0) {
        uputs("stat(/greeting.txt): type=");
        put_dec(st.type);
        uputs(" size=");
        put_dec(st.size);
        uputs("\n");
    } else {
        uputs("stat failed\n");
    }

    int fd = sys_open("/greeting.txt", O_RDONLY);
    if (fd >= 0) {
        char buf[128];
        long n = sys_read(fd, buf, sizeof(buf));
        uputs("read "); put_dec(n); uputs(" bytes: ");
        sys_write(1, buf, n);
        uputs("\n");
        sys_close(fd);
    } else {
        uputs("open failed\n");
    }

    /* Write + read round-trip. */
    int wfd = sys_open("/tmp.txt", O_WRONLY | O_CREAT);
    const char *payload = "round-trip OK\n";
    long wlen = 0; while (payload[wlen]) wlen++;
    sys_write(wfd, payload, wlen);
    sys_close(wfd);

    int rfd = sys_open("/tmp.txt", O_RDONLY);
    char rbuf[64];
    long rn = sys_read(rfd, rbuf, sizeof(rbuf));
    sys_close(rfd);
    uputs("wrote+read: ");
    sys_write(1, rbuf, rn);

    sys_unlink("/tmp.txt");
    if (sys_stat("/tmp.txt", &st) == 0) uputs("BUG: tmp.txt still exists\n");
    else                                 uputs("unlink OK\n");

    return 0;
}
