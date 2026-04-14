#include "syscall.h"
#include "ulib.h"

static uint32_t parse_hex(const char *s) {
    uint32_t v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    while (*s) {
        char c = *s++;
        uint32_t d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else break;
        v = (v << 4) | d;
    }
    return v;
}

static uint32_t parse_dec(const char *s) {
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return v;
}

static char hex_digit(unsigned n) {
    return (n < 10) ? ('0' + n) : ('a' + n - 10);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        puts("Usage: hexdump <hexaddr> [count]\n");
        puts("Reads from kernel memory (first 16MB only).\n");
        return 1;
    }
    uint32_t addr = parse_hex(argv[1]);
    uint32_t n = (argc > 2) ? parse_dec(argv[2]) : 128;

    unsigned char buf[16];
    for (uint32_t off = 0; off < n; off += 16) {
        uint32_t chunk = (n - off > 16) ? 16 : (n - off);
        int32_t got = sys_peek(addr + off, buf, chunk);
        if (got < 0) {
            printf("%x: <unreadable>\n", addr + off);
            break;
        }
        printf("%x: ", addr + off);
        for (uint32_t i = 0; i < (uint32_t)got; i++) {
            putchar(hex_digit((buf[i] >> 4) & 0xF));
            putchar(hex_digit(buf[i] & 0xF));
            putchar(' ');
        }
        for (uint32_t i = got; i < 16; i++) { putchar(' '); putchar(' '); putchar(' '); }
        putchar('|');
        for (uint32_t i = 0; i < (uint32_t)got; i++) {
            char c = (char)buf[i];
            putchar((c >= ' ' && c < 127) ? c : '.');
        }
        putchar('|');
        putchar('\n');
    }
    return 0;
}
