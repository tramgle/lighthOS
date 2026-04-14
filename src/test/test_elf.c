#include "test/test.h"
#include "kernel/elf.h"
#include "mm/vmm.h"

/* A minimal valid ELF32 header for testing validation */
static uint8_t good_elf[] = {
    /* ELF header (52 bytes) */
    0x7F, 'E', 'L', 'F',   /* magic */
    1,                       /* class: 32-bit */
    1,                       /* data: little-endian */
    1,                       /* version */
    0,                       /* OS/ABI */
    0,0,0,0,0,0,0,0,        /* padding */
    2, 0,                    /* type: ET_EXEC */
    3, 0,                    /* machine: EM_386 */
    1, 0, 0, 0,             /* version */
    0x00, 0x80, 0x04, 0x08, /* entry: 0x08048000 */
    52, 0, 0, 0,             /* phoff: 52 (right after header) */
    0, 0, 0, 0,             /* shoff */
    0, 0, 0, 0,             /* flags */
    52, 0,                   /* ehsize */
    32, 0,                   /* phentsize */
    1, 0,                    /* phnum: 1 */
    0, 0,                    /* shentsize */
    0, 0,                    /* shnum */
    0, 0,                    /* shstrndx */
    /* Program header (32 bytes) at offset 52 */
    1, 0, 0, 0,             /* type: PT_LOAD */
    84, 0, 0, 0,            /* offset: 84 (after headers) */
    0x00, 0x80, 0x04, 0x08, /* vaddr: 0x08048000 */
    0x00, 0x80, 0x04, 0x08, /* paddr */
    4, 0, 0, 0,             /* filesz: 4 bytes */
    4, 0, 0, 0,             /* memsz: 4 bytes */
    5, 0, 0, 0,             /* flags: PF_R | PF_X */
    0x00, 0x10, 0, 0,       /* align: 4096 */
    /* Segment data at offset 84 */
    0xCC, 0xCC, 0xCC, 0xCC, /* placeholder code bytes */
};

test_results_t test_elf(void) {
    TEST_SUITE_BEGIN("elf");

    /* Valid ELF accepted */
    TEST_ASSERT_EQ(elf_validate(good_elf, sizeof(good_elf)), 0, "valid ELF accepted");

    /* Bad magic rejected */
    uint8_t bad_magic[sizeof(good_elf)];
    memcpy(bad_magic, good_elf, sizeof(good_elf));
    bad_magic[0] = 0x00;
    TEST_ASSERT_NEQ(elf_validate(bad_magic, sizeof(bad_magic)), 0, "bad magic rejected");

    /* Wrong class (64-bit) rejected */
    uint8_t bad_class[sizeof(good_elf)];
    memcpy(bad_class, good_elf, sizeof(good_elf));
    bad_class[4] = 2;  /* ELFCLASS64 */
    TEST_ASSERT_NEQ(elf_validate(bad_class, sizeof(bad_class)), 0, "64-bit ELF rejected");

    /* Too small rejected */
    TEST_ASSERT_NEQ(elf_validate(good_elf, 10), 0, "truncated ELF rejected");

    /* elf_load returns correct entry point (loads into a fresh PD so the
       test doesn't perturb the kernel's address space). */
    uint32_t *test_pd = vmm_new_pd();
    TEST_ASSERT_NEQ((uint32_t)test_pd, 0, "vmm_new_pd returned a PD");
    uint32_t entry = elf_load(good_elf, sizeof(good_elf), test_pd);
    TEST_ASSERT_EQ(entry, 0x08048000, "elf_load returns correct entry point");
    vmm_free_pd(test_pd);

    TEST_SUITE_END();
}
