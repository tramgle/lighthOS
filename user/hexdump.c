/* hexdump <hexaddr> [count]
 * Reads `count` bytes from physical memory via SYS_PEEK. Source must
 * lie within the first 64 MiB (the kernel HHDM window). Default count
 * is 128. Output mimics `hexdump -C` — 16-byte rows with the ASCII
 * gutter on the right. */

#include "ulib_x64.h"

static uint64_t parse_hex(const char *s) {
    uint64_t v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    while (*s) {
        char c = *s++;
        unsigned d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        v = (v << 4) | d;
    }
    return v;
}

static void put_hex_padded(uint64_t v, int width) {
    char b[16]; int i = 0;
    if (v == 0) b[i++] = '0';
    while (v > 0) {
        unsigned d = (unsigned)(v & 0xF);
        b[i++] = d < 10 ? ('0' + d) : ('a' + d - 10);
        v >>= 4;
    }
    while (i < width) { u_putc('0'); width--; }
    while (i > 0) u_putc(b[--i]);
}

static void put_byte(unsigned char c) {
    const char hex[] = "0123456789abcdef";
    u_putc(hex[(c >> 4) & 0xF]);
    u_putc(hex[c & 0xF]);
}

int main(int argc, char **argv, char **envp) {
    (void)envp;
    if (argc < 2) {
        u_puts_n("usage: hexdump <hexaddr> [count]\n");
        return 1;
    }
    uint64_t addr = parse_hex(argv[1]);
    uint64_t total = (argc > 2) ? (uint64_t)u_atoi(argv[2]) : 128;

    unsigned char buf[16];
    for (uint64_t off = 0; off < total; off += 16) {
        uint64_t chunk = (total - off > 16) ? 16 : (total - off);
        long n = sys_peek(addr + off, buf, chunk);
        if (n < 0) { u_puts_n("hexdump: peek failed\n"); return 1; }

        put_hex_padded(addr + off, 8);
        u_puts_n("  ");
        for (uint64_t i = 0; i < 16; i++) {
            if (i < (uint64_t)n) { put_byte(buf[i]); u_putc(' '); }
            else                  u_puts_n("   ");
            if (i == 7) u_putc(' ');
        }
        u_puts_n(" |");
        for (long i = 0; i < n; i++) {
            char c = buf[i];
            u_putc((c >= ' ' && c < 0x7F) ? c : '.');
        }
        u_puts_n("|\n");
    }
    return 0;
}
