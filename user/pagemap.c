/* pagemap <hexaddr>
 * Walks the current process's PML4 for a user VA and prints each of
 * the four levels + the final physical address. */

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

static void hex(uint64_t v) { u_puts_n("0x"); u_puthex(v); }

int main(int argc, char **argv, char **envp) {
    (void)envp;
    if (argc < 2) {
        u_puts_n("usage: pagemap <hexaddr>\n");
        return 1;
    }
    uint64_t va = parse_hex(argv[1]);
    struct pagemap_out pm;
    if (sys_pagemap(va, &pm) != 0) {
        u_puts_n("pagemap: syscall failed\n");
        return 1;
    }
    u_puts_n("vaddr "); hex(va);
    u_puts_n("  pml4["); u_putdec(pm.pml4_idx);
    u_puts_n("] pdpt["); u_putdec(pm.pdpt_idx);
    u_puts_n("] pd[");   u_putdec(pm.pd_idx);
    u_puts_n("] pt[");   u_putdec(pm.pt_idx);
    u_puts_n("] offset="); hex(va & 0xFFF); u_putc('\n');

    u_puts_n("  PML4E "); hex(pm.pml4e); u_putc('\n');
    if (!(pm.pml4e & 1)) { u_puts_n("  (pml4 not present)\n"); return 0; }
    u_puts_n("  PDPTE "); hex(pm.pdpte); u_putc('\n');
    if (!(pm.pdpte & 1)) { u_puts_n("  (pdpt not present)\n"); return 0; }
    u_puts_n("  PDE   "); hex(pm.pde);   u_putc('\n');
    if (!(pm.pde & 1))   { u_puts_n("  (pd not present)\n");   return 0; }
    u_puts_n("  PTE   "); hex(pm.pte);   u_putc('\n');
    if (!(pm.pte & 1))   { u_puts_n("  (pt not present)\n");   return 0; }
    u_puts_n("  phys  "); hex(pm.phys);  u_putc('\n');
    return 0;
}
