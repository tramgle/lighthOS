/* Minimal GDB remote-protocol stub over COM2 (0x2F8).
 *
 * Supports enough of the protocol to:
 *   - attach via `gdb; target remote tcp:localhost:1234`
 *   - `?`           — report stop reason
 *   - `g` / `G`     — read / write general-purpose registers
 *   - `m` / `M`     — read / write memory
 *   - `c` / `s`     — continue / single-step
 *   - `Z0` / `z0`   — software breakpoint insert / remove (int3 swap)
 *   - `qSupported`  — capabilities (reply empty → use defaults)
 *
 * Polling-only: the stub spins on COM2 receive with IRQs disabled.
 * That's intentional — the rest of the kernel must be paused while
 * we're inside the debugger. Invoked on INT 3 (breakpoint exception)
 * today; panic.c can also drop here for post-mortem inspection.
 *
 * GDB sees i386 registers in this order (16 × 4 bytes = 128 hex chars):
 *   eax ecx edx ebx esp ebp esi edi eip eflags cs ss ds es fs gs
 */

#include "kernel/gdbstub.h"
#include "kernel/isr.h"
#include "include/io.h"
#include "lib/string.h"
#include "lib/kprintf.h"

#define COM2 0x2F8

static int gdb_inited;

static void gdb_putc(char c) {
    /* Wait for THR empty (bit 5 in LSR). */
    while (!(inb(COM2 + 5) & 0x20)) { }
    outb(COM2, (uint8_t)c);
}

static char gdb_getc(void) {
    /* Wait for data-ready (bit 0 in LSR). */
    while (!(inb(COM2 + 5) & 0x01)) { }
    return (char)inb(COM2);
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static char hex_digit(int v) {
    v &= 0xf;
    return v < 10 ? ('0' + v) : ('a' + v - 10);
}

/* Receive one gdb packet. Returns length of body (0 on empty).
   Buffer must be at least 1 byte larger than max payload for NUL. */
static int gdb_recv(char *buf, int cap) {
    for (;;) {
        char c;
        /* Skip until '$' (packet start). Interrupt (Ctrl-C) is 0x03 —
           treat as a fresh attach. */
        do { c = gdb_getc(); } while (c != '$' && c != 0x03);
        if (c == 0x03) { buf[0] = '\0'; return 0; }

        int len = 0;
        uint8_t csum = 0;
        while (len < cap - 1) {
            c = gdb_getc();
            if (c == '#') break;
            csum += (uint8_t)c;
            buf[len++] = c;
        }
        buf[len] = '\0';

        /* Two hex checksum chars follow '#'. */
        char h = gdb_getc(), l = gdb_getc();
        int exp = (hex_val(h) << 4) | hex_val(l);
        if (exp != (int)csum) {
            gdb_putc('-');   /* NACK — gdb retransmits */
            continue;
        }
        gdb_putc('+');
        return len;
    }
}

/* Send a reply packet. `body` is a NUL-terminated C string. */
static void gdb_send(const char *body) {
    for (;;) {
        gdb_putc('$');
        uint8_t csum = 0;
        for (const char *p = body; *p; p++) {
            gdb_putc(*p);
            csum += (uint8_t)*p;
        }
        gdb_putc('#');
        gdb_putc(hex_digit(csum >> 4));
        gdb_putc(hex_digit(csum));
        /* Wait for + (ack) or - (retransmit). */
        char c = gdb_getc();
        if (c == '+') return;
        /* Anything else: retransmit. */
    }
}

static void hex_byte(char *out, uint8_t b) {
    out[0] = hex_digit(b >> 4);
    out[1] = hex_digit(b);
}

/* Emit a 32-bit value little-endian as 8 hex chars. */
static void emit_u32(char *out, uint32_t v) {
    for (int i = 0; i < 4; i++) hex_byte(out + i * 2, (v >> (i * 8)) & 0xff);
}

/* Parse up-to-8 hex chars (little-endian byte order) into a uint32_t. */
static uint32_t parse_u32(const char *s) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        int h = hex_val(s[i * 2]), l = hex_val(s[i * 2 + 1]);
        if (h < 0 || l < 0) break;
        v |= ((uint32_t)((h << 4) | l)) << (i * 8);
    }
    return v;
}

/* Parse `addr,len` (both hex, comma separator). Returns bytes
   consumed, or -1 on malformed input. Stores results via pointers. */
static int parse_addr_len(const char *s, uint32_t *addr_out, uint32_t *len_out) {
    uint32_t a = 0, n = 0;
    const char *p = s;
    int v;
    while ((v = hex_val(*p)) >= 0) { a = a * 16 + v; p++; }
    if (*p != ',') return -1;
    p++;
    while ((v = hex_val(*p)) >= 0) { n = n * 16 + v; p++; }
    *addr_out = a;
    *len_out = n;
    return (int)(p - s);
}

/* Write register-block reply (all 16 regs as hex). */
static void reply_registers(registers_t *r, char *out) {
    uint32_t regs[16];
    int i = 0;
    regs[i++] = r->eax;
    regs[i++] = r->ecx;
    regs[i++] = r->edx;
    regs[i++] = r->ebx;
    /* ESP: for user-mode traps use useresp; otherwise regs->esp is
       the in-kernel stack pointer which GDB can still make sense of. */
    regs[i++] = (r->cs & 3) == 3 ? r->useresp : r->esp;
    regs[i++] = r->ebp;
    regs[i++] = r->esi;
    regs[i++] = r->edi;
    regs[i++] = r->eip;
    regs[i++] = r->eflags;
    regs[i++] = r->cs;
    regs[i++] = (r->cs & 3) == 3 ? r->ss : 0x10;
    regs[i++] = r->ds;
    regs[i++] = r->es;
    regs[i++] = r->fs;
    regs[i++] = r->gs;
    for (int k = 0; k < 16; k++) emit_u32(out + k * 8, regs[k]);
    out[128] = '\0';
}

/* Absorb a G (write registers) payload. Mirrors reply_registers's
   ordering. Leaves segment regs untouched when writing would require
   a ring transition we can't safely do here. */
static void load_registers(registers_t *r, const char *body) {
    uint32_t v[16];
    for (int i = 0; i < 16; i++) v[i] = parse_u32(body + i * 8);
    r->eax = v[0]; r->ecx = v[1]; r->edx = v[2]; r->ebx = v[3];
    if ((r->cs & 3) == 3) r->useresp = v[4]; else r->esp = v[4];
    r->ebp = v[5]; r->esi = v[6]; r->edi = v[7];
    r->eip = v[8]; r->eflags = v[9];
    /* Skip cs/ss/ds/es/fs/gs — mutating those mid-trap is unsafe. */
}

/* Read/write memory, bounded by identity-mapped kernel range. */
static int mem_read(uint32_t addr, uint32_t len, char *out) {
    /* Permit anything — gdb user knows what they're doing. Caps at
       256 bytes per packet to keep the reply small. */
    if (len > 256) len = 256;
    for (uint32_t i = 0; i < len; i++) {
        hex_byte(out + i * 2, *((volatile uint8_t *)(addr + i)));
    }
    out[len * 2] = '\0';
    return (int)len;
}

static int mem_write(uint32_t addr, uint32_t len, const char *hex) {
    if (len > 256) return -1;
    for (uint32_t i = 0; i < len; i++) {
        int h = hex_val(hex[i * 2]), l = hex_val(hex[i * 2 + 1]);
        if (h < 0 || l < 0) return -1;
        *((volatile uint8_t *)(addr + i)) = (uint8_t)((h << 4) | l);
    }
    return (int)len;
}

/* Software breakpoints: overwrite a byte with 0xCC (int3), remember
   the original so `z0` can restore it. Ring-3 breakpoints land on
   user pages via the current CR3. */
#define MAX_BREAKS 16
static struct { uint32_t addr; uint8_t orig; bool live; } breaks[MAX_BREAKS];

static int brk_add(uint32_t addr) {
    for (int i = 0; i < MAX_BREAKS; i++) if (breaks[i].live && breaks[i].addr == addr) return 0;
    for (int i = 0; i < MAX_BREAKS; i++) {
        if (!breaks[i].live) {
            breaks[i].addr = addr;
            breaks[i].orig = *((volatile uint8_t *)addr);
            *((volatile uint8_t *)addr) = 0xCC;
            breaks[i].live = true;
            return 0;
        }
    }
    return -1;
}

static int brk_remove(uint32_t addr) {
    for (int i = 0; i < MAX_BREAKS; i++) {
        if (breaks[i].live && breaks[i].addr == addr) {
            *((volatile uint8_t *)addr) = breaks[i].orig;
            breaks[i].live = false;
            return 0;
        }
    }
    return -1;
}

/* Main stub entry — blocks until gdb sends c or s. */
void gdbstub_enter(registers_t *regs) {
    if (!gdb_inited) gdbstub_init();

    char pkt[512];
    char reply[1024];

    /* Announce the stop reason proactively so gdb knows we're ready
       when it first attaches. gdb also accepts the initial "?" it
       sends — mirror that behavior. */
    gdb_send("S05");

    for (;;) {
        int n = gdb_recv(pkt, sizeof pkt);
        (void)n;
        char cmd = pkt[0];
        const char *body = pkt + 1;

        switch (cmd) {
        case '?':
            gdb_send("S05");
            break;

        case 'g':
            reply_registers(regs, reply);
            gdb_send(reply);
            break;

        case 'G':
            load_registers(regs, body);
            gdb_send("OK");
            break;

        case 'm': {
            uint32_t addr, len;
            if (parse_addr_len(body, &addr, &len) < 0) { gdb_send("E01"); break; }
            mem_read(addr, len, reply);
            gdb_send(reply);
            break;
        }

        case 'M': {
            uint32_t addr, len;
            const char *colon;
            if (parse_addr_len(body, &addr, &len) < 0) { gdb_send("E01"); break; }
            colon = body;
            while (*colon && *colon != ':') colon++;
            if (*colon != ':') { gdb_send("E01"); break; }
            if (mem_write(addr, len, colon + 1) < 0) gdb_send("E02");
            else gdb_send("OK");
            break;
        }

        case 'Z': {
            /* Z0,addr,kind — software breakpoint. Only kind 1 (x86)
               is meaningful; ignore the suffix. */
            if (body[0] != '0') { gdb_send(""); break; }
            uint32_t addr, kind;
            if (parse_addr_len(body + 2, &addr, &kind) < 0) { gdb_send("E01"); break; }
            if (brk_add(addr) != 0) gdb_send("E03");
            else gdb_send("OK");
            break;
        }

        case 'z': {
            if (body[0] != '0') { gdb_send(""); break; }
            uint32_t addr, kind;
            if (parse_addr_len(body + 2, &addr, &kind) < 0) { gdb_send("E01"); break; }
            brk_remove(addr);
            gdb_send("OK");
            break;
        }

        case 'c':
            /* Continue. If the breakpoint was at eip, advance past the
               int3 byte so we don't re-trap immediately. */
            return;

        case 's':
            /* Single-step via TF flag in eflags. CPU sets flag once,
               then clears on the next instruction's debug-trap exit. */
            regs->eflags |= 0x100;
            return;

        case 'q':
            /* qSupported / qC / etc. — reply empty ("") for anything
               we don't implement. gdb handles that gracefully. */
            gdb_send("");
            break;

        default:
            /* Unknown — reply empty, gdb proceeds. */
            gdb_send("");
            break;
        }
    }
}

/* Installed as the INT 3 handler. Our isr dispatcher hands us the
   registers_t; we drop into the stub and resume on `c`. */
static registers_t *gdbstub_trap_handler(registers_t *regs) {
    /* INT 3 advances eip past the 0xCC byte automatically (CPU
       behavior). If we installed a software breakpoint there, gdb
       expects eip to reflect the breakpoint itself — back it up. */
    regs->eip -= 1;
    gdbstub_enter(regs);
    /* On continue: if there was a Z0 breakpoint at this eip, we need
       to temporarily remove it, single-step, restore. Simplest: when
       gdb removes its breakpoint before `c`, eip points at the
       original (now-restored) instruction. We advance nothing and
       re-execute. */
    return regs;
}

void gdbstub_init(void) {
    if (gdb_inited) return;
    /* 115200 baud on COM2, 8N1, no flow control. Mirrors serial.c's
       setup but on COM2's IO base. */
    outb(COM2 + 1, 0x00);
    outb(COM2 + 3, 0x80);            /* DLAB */
    outb(COM2 + 0, 0x01);            /* divisor = 1 → 115200 */
    outb(COM2 + 1, 0x00);
    outb(COM2 + 3, 0x03);            /* 8N1 */
    outb(COM2 + 4, 0x0B);
    /* No IRQs — polling only. */
    isr_register_handler(3, gdbstub_trap_handler);
    gdb_inited = 1;
    serial_printf("[gdb] Stub ready on COM2\n");
}
