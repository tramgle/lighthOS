#include "kernel/elf.h"
#include "kernel/debug.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "lib/string.h"
#include "lib/kprintf.h"

int elf_validate(const void *data, uint32_t size) {
    if (size < sizeof(elf32_ehdr_t)) return -1;

    const elf32_ehdr_t *ehdr = (const elf32_ehdr_t *)data;

    if (ehdr->e_ident_magic != ELF_MAGIC) return -1;
    if (ehdr->e_ident_class != 1) return -1;   /* 32-bit */
    if (ehdr->e_ident_data != 1) return -1;    /* little-endian */
    /* ET_EXEC = statically loaded main executable (fixed p_vaddr).
       ET_DYN  = position-independent (shared lib, PIE interpreter).
       Both go through the same loader — ET_DYN callers pass a
       non-zero load_base to elf_load_at. */
    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN) return -1;
    if (ehdr->e_machine != EM_386) return -1;
    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) return -1;

    /* Verify program headers fit within the data */
    uint32_t ph_end = ehdr->e_phoff + ehdr->e_phnum * ehdr->e_phentsize;
    if (ph_end > size) return -1;

    return 0;
}

/* Copy a byte range into a target PD by walking page boundaries and
   writing through each frame's physical address. This lets us populate
   a page directory that isn't the live CR3 — essential because spawn
   happens in the parent's kernel context, while the target PD belongs
   to the not-yet-scheduled child. */
static void write_into_pd(uint32_t *pd, uint32_t vaddr, const uint8_t *src, uint32_t n) {
    while (n > 0) {
        uint32_t page = vaddr & ~(PAGE_SIZE - 1);
        uint32_t off  = vaddr - page;
        uint32_t chunk = PAGE_SIZE - off;
        if (chunk > n) chunk = n;

        uint32_t phys = vmm_get_physical_in(pd, page);
        if (!phys) {
            /* Caller guaranteed the pages are mapped. */
            return;
        }
        memcpy((void *)(phys + off), src, chunk);
        vaddr += chunk;
        src   += chunk;
        n     -= chunk;
    }
}

static void zero_into_pd(uint32_t *pd, uint32_t vaddr, uint32_t n) {
    while (n > 0) {
        uint32_t page = vaddr & ~(PAGE_SIZE - 1);
        uint32_t off  = vaddr - page;
        uint32_t chunk = PAGE_SIZE - off;
        if (chunk > n) chunk = n;

        uint32_t phys = vmm_get_physical_in(pd, page);
        if (!phys) return;
        memset((void *)(phys + off), 0, chunk);
        vaddr += chunk;
        n     -= chunk;
    }
}

uint32_t elf_load_at(const void *data, uint32_t size, uint32_t *pd,
                     uint32_t load_base) {
    if (!pd) return 0;
    if (elf_validate(data, size) != 0) return 0;

    const elf32_ehdr_t *ehdr = (const elf32_ehdr_t *)data;
    const uint8_t *bytes = (const uint8_t *)data;

    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const elf32_phdr_t *phdr = (const elf32_phdr_t *)(bytes + ehdr->e_phoff + i * ehdr->e_phentsize);

        if (phdr->p_type != PT_LOAD) continue;
        if (phdr->p_memsz == 0) continue;

        /* Validate segment fits in file. Explicit overflow guards so a
           crafted ELF with e.g. p_offset=0xFFFFFF00, p_filesz=0x200
           can't wrap to a value that looks <= size. */
        if (phdr->p_offset + phdr->p_filesz < phdr->p_offset) {
            serial_printf("[elf] Segment %u p_offset+p_filesz overflow\n", i);
            return 0;
        }
        if (phdr->p_offset + phdr->p_filesz > size) {
            serial_printf("[elf] Segment %u extends past file end\n", i);
            return 0;
        }
        if (phdr->p_filesz > phdr->p_memsz) {
            serial_printf("[elf] Segment %u filesz > memsz\n", i);
            return 0;
        }

        /* Effective virtual address after relocation. ET_DYN files
           typically have p_vaddr near 0; the load_base offset puts
           them in the chosen address window. */
        uint32_t vaddr = phdr->p_vaddr + load_base;
        if (vaddr < phdr->p_vaddr) {
            serial_printf("[elf] Segment %u p_vaddr+load_base overflow\n", i);
            return 0;
        }

        /* Reject segments that would straddle the kernel address space.
           Our user convention: everything must live in [0x08000000,
           0xC0000000). A segment pointing below 0x08000000 would make
           vmm_map_in install USER pages inside kernel PDEs, letting
           the ELF shadow kernel memory. */
        if (vaddr < 0x08000000u) {
            serial_printf("[elf] Segment %u effective vaddr 0x%x below user base\n",
                          i, vaddr);
            return 0;
        }
        if (phdr->p_memsz > 0x10000000u) {        /* > 256 MB is absurd here */
            serial_printf("[elf] Segment %u p_memsz %u implausibly large\n",
                          i, phdr->p_memsz);
            return 0;
        }
        if (vaddr + phdr->p_memsz < vaddr) {
            serial_printf("[elf] Segment %u vaddr+p_memsz overflow\n", i);
            return 0;
        }
        if (vaddr + phdr->p_memsz > 0xC0000000u) {
            serial_printf("[elf] Segment %u ends past user-space limit\n", i);
            return 0;
        }

        /* Always allocate a fresh frame per page. Under per-process PDs
           the child's address space starts empty above the kernel PDEs,
           so there's no "reuse the existing mapping" case to optimize. */
        uint32_t vaddr_start = vaddr & ~(PAGE_SIZE - 1);
        uint32_t vaddr_end = (vaddr + phdr->p_memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

        for (uint32_t addr = vaddr_start; addr < vaddr_end; addr += PAGE_SIZE) {
            /* Skip pages already mapped — this happens when the main
               exec and the dynamic interpreter both cover the same
               page at a PT_LOAD boundary (extremely unlikely here but
               safe to handle). */
            if (vmm_get_physical_in(pd, addr)) continue;
            uint32_t frame = pmm_alloc_frame();
            if (!frame) {
                serial_printf("[elf] Out of memory mapping segment\n");
                return 0;
            }
            /* Zero the frame via its identity-mapped physical address.
               Writing at the ELF virtual address wouldn't work when pd
               isn't the live CR3. */
            memset((void *)frame, 0, PAGE_SIZE);
            vmm_map_in(pd, addr, frame, VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_USER);
        }

        /* Copy file data into the freshly mapped pages. */
        if (phdr->p_filesz > 0) {
            write_into_pd(pd, vaddr, bytes + phdr->p_offset, phdr->p_filesz);
        }

        /* Zero BSS (the region between filesz and memsz). The frames
           were pre-zeroed above, but be explicit — partial-page BSS
           starts on the same page as the tail of the file data. */
        if (phdr->p_memsz > phdr->p_filesz) {
            zero_into_pd(pd, vaddr + phdr->p_filesz,
                         phdr->p_memsz - phdr->p_filesz);
        }

        dlog("[elf] Loaded segment: vaddr=0x%x filesz=%u memsz=%u\n",
             vaddr, phdr->p_filesz, phdr->p_memsz);
    }

    return ehdr->e_entry + load_base;
}

uint32_t elf_load(const void *data, uint32_t size, uint32_t *pd) {
    return elf_load_at(data, size, pd, 0);
}

int elf_find_interp(const void *data, uint32_t size, char *out, uint32_t cap) {
    if (!out || cap == 0) return 0;
    if (elf_validate(data, size) != 0) return 0;

    const elf32_ehdr_t *ehdr = (const elf32_ehdr_t *)data;
    const uint8_t *bytes = (const uint8_t *)data;

    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const elf32_phdr_t *phdr = (const elf32_phdr_t *)(bytes + ehdr->e_phoff + i * ehdr->e_phentsize);
        if (phdr->p_type != PT_INTERP) continue;
        /* Bounds-check the file-offset range. */
        if (phdr->p_offset + phdr->p_filesz < phdr->p_offset) return 0;
        if (phdr->p_offset + phdr->p_filesz > size) return 0;
        if (phdr->p_filesz == 0) return 0;

        uint32_t n = phdr->p_filesz;
        if (n >= cap) n = cap - 1;
        memcpy(out, bytes + phdr->p_offset, n);
        /* Strip any trailing NULs included in p_filesz, then terminate. */
        while (n > 0 && out[n - 1] == '\0') n--;
        out[n] = '\0';
        return n > 0 ? 1 : 0;
    }
    return 0;
}

uint32_t elf_phdr_vaddr(const void *data, uint32_t size) {
    if (elf_validate(data, size) != 0) return 0;
    const elf32_ehdr_t *ehdr = (const elf32_ehdr_t *)data;
    const uint8_t *bytes = (const uint8_t *)data;

    /* Find the PT_LOAD segment whose file range covers e_phoff. The
       runtime vaddr of the phdrs is then p_vaddr + (e_phoff - p_offset). */
    for (uint16_t i = 0; i < ehdr->e_phnum; i++) {
        const elf32_phdr_t *phdr = (const elf32_phdr_t *)(bytes + ehdr->e_phoff + i * ehdr->e_phentsize);
        if (phdr->p_type != PT_LOAD) continue;
        if (ehdr->e_phoff >= phdr->p_offset &&
            ehdr->e_phoff < phdr->p_offset + phdr->p_filesz) {
            return phdr->p_vaddr + (ehdr->e_phoff - phdr->p_offset);
        }
    }
    return 0;
}
