#ifndef ELF_H
#define ELF_H

#include "include/types.h"

#define ELF_MAGIC   0x464C457F       /* "\x7FELF" little-endian */
#define ELFCLASS64  2
#define ELFDATA2LSB 1

#define ET_EXEC   2
#define ET_DYN    3
#define EM_X86_64 62

#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_INTERP  3
#define PT_PHDR    6

#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4

typedef struct {
    uint32_t e_ident_magic;
    uint8_t  e_ident_class;     /* 2 = 64-bit (ELFCLASS64) */
    uint8_t  e_ident_data;
    uint8_t  e_ident_version;
    uint8_t  e_ident_osabi;
    uint8_t  e_ident_pad[8];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) elf64_ehdr_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;       /* field-order swap from ELF32 */
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed)) elf64_phdr_t;

int      elf_validate(const void *data, uint64_t size);
uint64_t elf_load_at(const void *data, uint64_t size, uint64_t *pml4,
                     uint64_t load_base);
uint64_t elf_load(const void *data, uint64_t size, uint64_t *pml4);
int      elf_find_interp(const void *data, uint64_t size, char *out, uint32_t cap);
uint64_t elf_phdr_vaddr(const void *data, uint64_t size);

#endif
