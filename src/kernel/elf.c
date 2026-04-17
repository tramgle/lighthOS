/* ELF64 loader.
 *
 * Loads ET_EXEC and ET_DYN binaries into a target PML4. Caller
 * allocates the PML4 (typically via vmm_new_pml4) and passes it
 * in. Each PT_LOAD segment gets fresh pages from pmm, mapped with
 * USER + appropriate R/W/X flags, zero-filled, then populated
 * from the file.
 *
 * The "write_into_pd" trick is the x86_64 version of the i386
 * original: we edit pages in the target PML4 via their physical
 * address + the identity map, so the loader doesn't need the
 * target PML4 to be the live CR3.
 *
 * User VA range sanity bound: [0x00010000, 0x00007FFFFFFFFFFF].
 * Bottom guards against NULL-page spray; top keeps below the
 * canonical-hole boundary so no segment straddles the sign-
 * extension gap.
 */

#include "kernel/elf.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "lib/string.h"
#include "lib/kprintf.h"

#define USER_VA_MIN 0x0000000000010000ULL
#define USER_VA_MAX 0x00007FFFFFFFFFFFULL

int elf_validate(const void *data, uint64_t size) {
    if (size < sizeof(elf64_ehdr_t)) return -1;
    const elf64_ehdr_t *ehdr = data;

    if (ehdr->e_ident_magic != ELF_MAGIC) return -1;
    if (ehdr->e_ident_class != ELFCLASS64) return -1;
    if (ehdr->e_ident_data  != ELFDATA2LSB) return -1;
    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN) return -1;
    if (ehdr->e_machine != EM_X86_64) return -1;
    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) return -1;
    /* Reject phdr tables whose entry size is smaller than the struct
       we're going to read through — otherwise a crafted ELF could
       overlap entries and bypass the per-phdr bounds checks below. */
    if (ehdr->e_phentsize < sizeof(elf64_phdr_t)) return -1;
    uint64_t ph_end = ehdr->e_phoff + (uint64_t)ehdr->e_phnum * ehdr->e_phentsize;
    if (ph_end < ehdr->e_phoff) return -1;
    if (ph_end > size) return -1;
    return 0;
}

static void write_into_pml4(uint64_t *pml4, uint64_t vaddr,
                            const uint8_t *src, uint64_t n) {
    while (n > 0) {
        uint64_t page  = vaddr & ~(uint64_t)(PAGE_SIZE - 1);
        uint64_t off   = vaddr - page;
        uint64_t chunk = PAGE_SIZE - off;
        if (chunk > n) chunk = n;
        uint64_t phys = vmm_get_physical_in(pml4, page);
        if (!phys) return;
        memcpy(phys_to_virt_low(phys + off), src, (uint32_t)chunk);
        vaddr += chunk; src += chunk; n -= chunk;
    }
}

static void zero_into_pml4(uint64_t *pml4, uint64_t vaddr, uint64_t n) {
    while (n > 0) {
        uint64_t page  = vaddr & ~(uint64_t)(PAGE_SIZE - 1);
        uint64_t off   = vaddr - page;
        uint64_t chunk = PAGE_SIZE - off;
        if (chunk > n) chunk = n;
        uint64_t phys = vmm_get_physical_in(pml4, page);
        if (!phys) return;
        memset(phys_to_virt_low(phys + off), 0, (uint32_t)chunk);
        vaddr += chunk; n -= chunk;
    }
}

uint64_t elf_load_at(const void *data, uint64_t size, uint64_t *pml4,
                     uint64_t load_base) {
    if (!pml4) return 0;
    if (elf_validate(data, size) != 0) return 0;
    const elf64_ehdr_t *ehdr = data;
    const uint8_t *bytes = data;

    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const elf64_phdr_t *phdr =
            (const elf64_phdr_t *)(bytes + ehdr->e_phoff + i * ehdr->e_phentsize);
        if (phdr->p_type != PT_LOAD || phdr->p_memsz == 0) continue;

        if (phdr->p_offset + phdr->p_filesz < phdr->p_offset)    return 0;
        if (phdr->p_offset + phdr->p_filesz > size)              return 0;
        if (phdr->p_filesz > phdr->p_memsz)                      return 0;

        uint64_t vaddr = phdr->p_vaddr + load_base;
        if (vaddr < phdr->p_vaddr) return 0;
        if (vaddr < USER_VA_MIN) {
            serial_printf("[elf] segment %u vaddr 0x%lx below user floor\n", i, vaddr);
            return 0;
        }
        if (vaddr + phdr->p_memsz < vaddr) return 0;
        if (vaddr + phdr->p_memsz > USER_VA_MAX) {
            serial_printf("[elf] segment %u ends past user ceiling\n", i);
            return 0;
        }

        uint64_t vstart = vaddr & ~(uint64_t)(PAGE_SIZE - 1);
        uint64_t vend   = (vaddr + phdr->p_memsz + PAGE_SIZE - 1)
                           & ~(uint64_t)(PAGE_SIZE - 1);

        for (uint64_t a = vstart; a < vend; a += PAGE_SIZE) {
            if (vmm_get_physical_in(pml4, a)) continue;
            uint64_t frame = pmm_alloc_frame();
            if (!frame) { serial_printf("[elf] OOM\n"); return 0; }
            memset(phys_to_virt_low(frame), 0, PAGE_SIZE);
            uint64_t flags = VMM_FLAG_WRITE | VMM_FLAG_USER;
            /* Respect X permission later; for L4 we grant RWX to keep
               things simple until L5 does proper SMEP/NX. */
            vmm_map_in(pml4, a, frame, flags);
        }

        if (phdr->p_filesz > 0) {
            write_into_pml4(pml4, vaddr, bytes + phdr->p_offset, phdr->p_filesz);
        }
        if (phdr->p_memsz > phdr->p_filesz) {
            zero_into_pml4(pml4, vaddr + phdr->p_filesz,
                           phdr->p_memsz - phdr->p_filesz);
        }
    }

    return ehdr->e_entry + load_base;
}

uint64_t elf_load(const void *data, uint64_t size, uint64_t *pml4) {
    return elf_load_at(data, size, pml4, 0);
}

int elf_find_interp(const void *data, uint64_t size, char *out, uint32_t cap) {
    if (!out || cap == 0) return 0;
    if (elf_validate(data, size) != 0) return 0;
    const elf64_ehdr_t *ehdr = data;
    const uint8_t *bytes = data;

    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const elf64_phdr_t *phdr =
            (const elf64_phdr_t *)(bytes + ehdr->e_phoff + i * ehdr->e_phentsize);
        if (phdr->p_type != PT_INTERP) continue;
        if (phdr->p_offset + phdr->p_filesz < phdr->p_offset) return 0;
        if (phdr->p_offset + phdr->p_filesz > size) return 0;
        if (phdr->p_filesz == 0) return 0;
        uint64_t n = phdr->p_filesz;
        if (n >= cap) n = cap - 1;
        memcpy(out, bytes + phdr->p_offset, (uint32_t)n);
        while (n > 0 && out[n - 1] == '\0') n--;
        out[n] = '\0';
        return n > 0 ? 1 : 0;
    }
    return 0;
}

uint64_t elf_phdr_vaddr(const void *data, uint64_t size) {
    if (elf_validate(data, size) != 0) return 0;
    const elf64_ehdr_t *ehdr = data;
    const uint8_t *bytes = data;
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const elf64_phdr_t *phdr =
            (const elf64_phdr_t *)(bytes + ehdr->e_phoff + i * ehdr->e_phentsize);
        if (phdr->p_type != PT_LOAD) continue;
        if (ehdr->e_phoff >= phdr->p_offset &&
            ehdr->e_phoff < phdr->p_offset + phdr->p_filesz) {
            return phdr->p_vaddr + (ehdr->e_phoff - phdr->p_offset);
        }
    }
    return 0;
}
