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

int main(int argc, char **argv) {
    if (argc < 2) {
        puts("Usage: pagemap <hexaddr>\n");
        return 1;
    }
    uint32_t va = parse_hex(argv[1]);
    struct pagemap_info pm;
    if (sys_pagemap(va, &pm) != 0) {
        puts("pagemap: syscall failed\n");
        return 1;
    }
    printf("vaddr 0x%x  pd_idx=%u  pt_idx=%u  offset=0x%x\n",
           va, pm.pd_idx, pm.pt_idx, va & 0xFFF);
    printf("PDE[%u]=0x%x  ", pm.pd_idx, pm.pde);
    if (!(pm.pde & 1)) { puts("not present\n"); return 0; }
    printf("{P W=%u U=%u PT=0x%x}\n",
           (pm.pde >> 1) & 1, (pm.pde >> 2) & 1, pm.pde & 0xFFFFF000);
    printf("PTE[%u]=0x%x  ", pm.pt_idx, pm.pte);
    if (!(pm.pte & 1)) { puts("not present\n"); return 0; }
    printf("{P W=%u U=%u frame=0x%x}  phys=0x%x\n",
           (pm.pte >> 1) & 1, (pm.pte >> 2) & 1, pm.pte & 0xFFFFF000, pm.phys);
    return 0;
}
