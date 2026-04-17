#ifndef SYSCALL_H
#define SYSCALL_H

#include "include/types.h"

#define SYS_EXIT    1
#define SYS_READ    3
#define SYS_WRITE   4
#define SYS_OPEN    5
#define SYS_CLOSE   6
#define SYS_WAITPID 7
#define SYS_UNLINK  10
#define SYS_CHDIR   12
#define SYS_STAT    18
#define SYS_GETPID  20
#define SYS_YIELD   24
#define SYS_MKDIR   39
#define SYS_SBRK    45
#define SYS_LSEEK   19
#define SYS_DUP2    63
#define SYS_FORK    57
#define SYS_EXECVE  59
#define SYS_READDIR 89
#define SYS_SPAWN   120
#define SYS_GETCWD  183
#define SYS_PS      200
#define SYS_SHUTDOWN 201
#define SYS_MEMINFO 210
#define SYS_REGIONS 211
#define SYS_PAGEMAP 212
#define SYS_PEEK    213
#define SYS_TIME    214
#define SYS_BLKDEVS 215
#define SYS_PIPE    42
#define SYS_CHROOT  161
#define SYS_KILL    37
#define SYS_SETPGID 109
#define SYS_GETPGID 108
#define SYS_SIGNAL    48
#define SYS_SIGRETURN 119
#define SYS_MOUNT     21
#define SYS_UMOUNT    22
#define SYS_ALARM     27
#define SYS_TRACEME     231
#define SYS_TRACE_READ  232
#define SYS_MMAP_ANON   9
#define SYS_MPROTECT    125
#define SYS_TTY_RAW     54   /* 0 = cooked (default), 1 = raw. */
#define SYS_TCSETPGRP   55   /* Set terminal foreground pgid. */

/* mmap/mprotect protection flags. PROT_EXEC is documented but not
   enforced — our paging setup has no NX bit at the i386 level. */
#define PROT_READ    0x1
#define PROT_WRITE   0x2
#define PROT_EXEC    0x4

#define SYSCALL_MAX 256

/* SYS_PAGEMAP output. For a given user VA, reports each level's
   index + the raw PTE-ish entry at that level + the final physical
   address (or 0 if not mapped). On x86_64 we have 4 levels, so
   pml4/pdpt/pd/pt all appear. */
struct pagemap_out {
    uint32_t pml4_idx;
    uint32_t pdpt_idx;
    uint32_t pd_idx;
    uint32_t pt_idx;
    uint64_t pml4e;
    uint64_t pdpte;
    uint64_t pde;
    uint64_t pte;
    uint64_t phys;
};

/* SYS_REGIONS output. One physical region from the PMM's ranged view.
   Caller invokes with idx=0, 1, 2, ... until syscall returns -1. */
struct region_out {
    uint64_t start_addr;
    uint64_t end_addr;
    uint32_t used;
    uint32_t _pad;
};

void syscall_init(void);

#endif
