#ifndef ELF_H
#define ELF_H

#include "include/types.h"

#define ELF_MAGIC 0x464C457F  /* "\x7FELF" little-endian */

#define ET_EXEC 2
#define EM_386  3
#define PT_LOAD 1

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

/* Validate an ELF header. Returns 0 on success, -1 on error. */
int elf_validate(const void *data, uint32_t size);

/* Load an ELF into the supplied page directory. Allocates fresh user
   frames for every page in every LOAD segment, copies file contents via
   physical addresses (so the target PD need not be the live CR3), and
   zeros the BSS tail. Returns the entry point, or 0 on failure. */
uint32_t elf_load(const void *data, uint32_t size, uint32_t *pd);

#endif
