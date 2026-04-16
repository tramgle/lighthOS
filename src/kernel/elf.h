#ifndef ELF_H
#define ELF_H

#include "include/types.h"

#define ELF_MAGIC 0x464C457F  /* "\x7FELF" little-endian */

#define ET_EXEC 2
#define ET_DYN  3
#define EM_386  3
#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_INTERP  3
#define PT_PHDR    6

#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4

typedef struct {
    uint32_t e_ident_magic;
    uint8_t  e_ident_class;     /* 1 = 32-bit */
    uint8_t  e_ident_data;      /* 1 = little-endian */
    uint8_t  e_ident_version;
    uint8_t  e_ident_osabi;
    uint8_t  e_ident_pad[8];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) elf32_ehdr_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} __attribute__((packed)) elf32_phdr_t;

/* Validate an ELF header. Accepts both ET_EXEC (main executables —
   fixed load address via p_vaddr) and ET_DYN (shared objects and
   PIE interpreters — relocatable via load_base). Returns 0 on
   success, -1 on error. */
int elf_validate(const void *data, uint32_t size);

/* Load an ELF into the supplied page directory with each PT_LOAD
   offset by `load_base`. For ET_EXEC callers pass 0 (original
   behavior). For ET_DYN (e.g. the ld-lighthos.so.1 interpreter) pass
   the chosen base address (e.g. 0x40000000). Returns the entry
   point (already offset by load_base), or 0 on failure. */
uint32_t elf_load_at(const void *data, uint32_t size, uint32_t *pd,
                     uint32_t load_base);

/* Thin wrapper: elf_load_at(data, size, pd, 0). Kept as the
   canonical call site for ET_EXEC loads. */
uint32_t elf_load(const void *data, uint32_t size, uint32_t *pd);

/* Scan program headers for PT_INTERP and copy the interpreter path
   into `out` (NUL-terminated, truncated to `cap - 1`). Returns 1 if
   PT_INTERP was found, 0 otherwise. Used by process_spawn / execve
   to decide whether to load a dynamic linker alongside the main ELF. */
int elf_find_interp(const void *data, uint32_t size, char *out, uint32_t cap);

/* Return the vaddr the program header table will occupy at runtime —
   i.e. the vaddr of the file offset e_phoff within the PT_LOAD that
   covers it. Needed for the auxv AT_PHDR entry. Returns 0 if phdrs
   aren't covered by any PT_LOAD (malformed ELF). */
uint32_t elf_phdr_vaddr(const void *data, uint32_t size);

#endif
