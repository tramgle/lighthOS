/* ld-lighthos.so.1 — x86_64 user-space dynamic linker.
 *
 * Loaded by the kernel at a fixed higher VA (0x40000000). Given a
 * SysV AMD64 stack (argc, argv, envp, auxv), we:
 *
 *   1. Parse auxv for AT_PHDR/AT_PHNUM/AT_ENTRY/AT_BASE.
 *   2. Build a loaded_obj_t for the main exec.
 *   3. Walk main's PT_DYNAMIC → for each DT_NEEDED, load the .so
 *      from /lib/ via sys_open+mmap_anon+memcpy, recursively.
 *   4. Apply each library's own R_X86_64_RELATIVE so its GOT is
 *      self-consistent.
 *   5. Apply main's + libraries' GLOB_DAT / JUMP_SLOT / 64 /
 *      PC32 / PLT32 / COPY relocations.
 *   6. Publish the dlopen/dlsym ops table at DL_IFACE_ADDR.
 *   7. Return main's entry to crt0_ldso which jumps there.
 *
 * Relocations use RELA (with explicit r_addend) on x86_64, unlike
 * i386's REL which stored the addend in place.
 */

#include "syscall.h"
#include "ulib.h"

/* --- ELF64 ---------------------------------------------------- */

typedef struct {
    uint32_t e_ident_magic;
    uint8_t  e_ident_class, e_ident_data, e_ident_version, e_ident_osabi;
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
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed)) elf64_phdr_t;

typedef struct {
    int64_t  d_tag;
    uint64_t d_val;
} __attribute__((packed)) elf64_dyn_t;

typedef struct {
    uint32_t st_name;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
} __attribute__((packed)) elf64_sym_t;

typedef struct {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t  r_addend;
} __attribute__((packed)) elf64_rela_t;

#define ELF64_R_TYPE(i) ((uint32_t)(i))
#define ELF64_R_SYM(i)  ((uint32_t)((i) >> 32))

#define ELF_MAGIC 0x464C457FU
#define ET_EXEC   2
#define ET_DYN    3

#define PT_LOAD    1
#define PT_DYNAMIC 2

#define DT_NULL     0
#define DT_NEEDED   1
#define DT_PLTRELSZ 2
#define DT_PLTGOT   3
#define DT_HASH     4
#define DT_STRTAB   5
#define DT_SYMTAB   6
#define DT_STRSZ    10
#define DT_SYMENT   11
#define DT_PLTREL   20
#define DT_JMPREL   23
#define DT_RELA     7
#define DT_RELASZ   8
#define DT_RELAENT  9

#define R_X86_64_NONE        0
#define R_X86_64_64          1
#define R_X86_64_PC32        2
#define R_X86_64_PLT32       4
#define R_X86_64_COPY        5
#define R_X86_64_GLOB_DAT    6
#define R_X86_64_JUMP_SLOT   7
#define R_X86_64_RELATIVE    8

#define AT_NULL    0
#define AT_PHDR    3
#define AT_PHENT   4
#define AT_PHNUM   5
#define AT_BASE    7
#define AT_ENTRY   9
#define AT_PAGESZ  6

/* Where to start loading shared libraries. First-fit ascending. */
#define DYN_REGION_START 0x30000000ULL
#define DYN_REGION_END   0x3FFFFFFFULL
#define MAX_LIBS 8

typedef struct {
    uint64_t      base;         /* load offset: runtime vaddr = base + link vaddr */
    uint64_t      span;
    char          path[128];
    elf64_dyn_t  *dyn;
    elf64_sym_t  *symtab;
    const char   *strtab;
    uint64_t      strsz;
    uint64_t      nsyms;
    elf64_rela_t *rela;          uint64_t relasz;
    elf64_rela_t *jmprela;       uint64_t pltrelsz;
    uint64_t     *hash;
} loaded_obj_t;

static loaded_obj_t g_libs[MAX_LIBS];
static int          g_nlibs;
static uint64_t     g_dyn_next = DYN_REGION_START;
static loaded_obj_t *g_main;

static int load_needed(loaded_obj_t *obj);

static void die(const char *msg) {
    puts("ld.so: "); puts(msg); puts("\n");
    sys_exit(127);
}

/* --- auxv helper -------------------------------------------- */

static uint64_t find_auxv(uint64_t *stack, uint64_t type) {
    /* stack[0]=argc, stack[1..argc]=argv ptrs, stack[argc+1]=0,
       envp ptrs, 0 terminator, then auxv pairs (key,val)... */
    uint64_t argc = stack[0];
    uint64_t *p = &stack[1 + argc + 1];
    while (*p) p++;
    p++;
    while (*p != AT_NULL) {
        if (*p == type) return p[1];
        p += 2;
    }
    return 0;
}

static void parse_dynamic(loaded_obj_t *obj) {
    for (elf64_dyn_t *d = obj->dyn; d->d_tag != DT_NULL; d++) {
        uint64_t v = d->d_val;
        switch (d->d_tag) {
        case DT_SYMTAB:   obj->symtab   = (elf64_sym_t *)(obj->base + v); break;
        case DT_STRTAB:   obj->strtab   = (const char  *)(obj->base + v); break;
        case DT_STRSZ:    obj->strsz    = v; break;
        case DT_RELA:     obj->rela     = (elf64_rela_t *)(obj->base + v); break;
        case DT_RELASZ:   obj->relasz   = v; break;
        case DT_JMPREL:   obj->jmprela  = (elf64_rela_t *)(obj->base + v); break;
        case DT_PLTRELSZ: obj->pltrelsz = v; break;
        case DT_HASH:     obj->hash     = (uint64_t *)(obj->base + v); break;
        default: break;
        }
    }
    /* DT_HASH buckets: nbucket(32), nchain(32), then arrays.
       nchain == nsyms — use it if present. The layout on 64-bit
       still uses 32-bit buckets/chains; cast accordingly. */
    if (obj->hash) {
        uint32_t *h32 = (uint32_t *)obj->hash;
        obj->nsyms = h32[1];
    }
}

static uint64_t page_up(uint64_t v) { return (v + 0xFFFULL) & ~0xFFFULL; }

/* Load an ET_DYN from `path` into the next free dyn slot. */
static int load_so(const char *path, loaded_obj_t *out) {
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) { puts("ld.so: cannot open "); puts(path); puts("\n"); return -1; }

    elf64_ehdr_t ehdr;
    if (sys_read(fd, &ehdr, sizeof ehdr) != (long)sizeof ehdr) { sys_close(fd); return -1; }
    if (ehdr.e_ident_magic != ELF_MAGIC) { sys_close(fd); return -1; }
    if (ehdr.e_type != ET_DYN)           { sys_close(fd); return -1; }
    if (ehdr.e_phnum == 0 || ehdr.e_phnum > 16) { sys_close(fd); return -1; }

    elf64_phdr_t phdrs[16];
    if (sys_lseek(fd, (long)ehdr.e_phoff, 0) < 0) { sys_close(fd); return -1; }
    long phsize = (long)ehdr.e_phnum * (long)sizeof(elf64_phdr_t);
    if (sys_read(fd, phdrs, phsize) != phsize) { sys_close(fd); return -1; }

    uint64_t min_v = ~(uint64_t)0, max_v = 0;
    for (int i = 0; i < ehdr.e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;
        uint64_t s = phdrs[i].p_vaddr & ~0xFFFULL;
        uint64_t e = page_up(phdrs[i].p_vaddr + phdrs[i].p_memsz);
        if (s < min_v) min_v = s;
        if (e > max_v) max_v = e;
    }
    if (min_v == ~(uint64_t)0) { sys_close(fd); return -1; }
    uint64_t span = max_v - min_v;

    uint64_t region = g_dyn_next;
    if (region + span > DYN_REGION_END) { sys_close(fd); return -1; }
    if (sys_mmap_anon((void *)(uintptr_t)region, span,
                      PROT_READ | PROT_WRITE) != (long)region) {
        sys_close(fd); return -1;
    }
    uint64_t base = region - min_v;
    g_dyn_next = (region + span + 0xFFFFFULL) & ~0xFFFFFULL;    /* 1 MiB align */

    for (int i = 0; i < ehdr.e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;
        uint64_t dest = base + phdrs[i].p_vaddr;
        if (phdrs[i].p_filesz > 0) {
            if (sys_lseek(fd, (long)phdrs[i].p_offset, 0) < 0) { sys_close(fd); return -1; }
            if (sys_read(fd, (void *)(uintptr_t)dest, phdrs[i].p_filesz) !=
                (long)phdrs[i].p_filesz) {
                sys_close(fd); return -1;
            }
        }
    }

    elf64_dyn_t *dyn = 0;
    for (int i = 0; i < ehdr.e_phnum; i++) {
        if (phdrs[i].p_type == PT_DYNAMIC) {
            dyn = (elf64_dyn_t *)(uintptr_t)(base + phdrs[i].p_vaddr);
            break;
        }
    }
    sys_close(fd);
    if (!dyn) return -1;

    out->base = base; out->span = span;
    int i = 0;
    while (path[i] && i < (int)sizeof(out->path) - 1) { out->path[i] = path[i]; i++; }
    out->path[i] = 0;
    out->dyn = dyn;
    out->symtab = 0; out->strtab = 0; out->rela = 0; out->jmprela = 0;
    out->hash = 0; out->nsyms = 0; out->strsz = 0; out->relasz = 0;
    out->pltrelsz = 0;
    parse_dynamic(out);
    return 0;
}

#define STT_NOTYPE 0
#define STT_OBJECT 1
#define STT_FUNC   2

static uint64_t lookup_in(loaded_obj_t *o, const char *name) {
    if (!o || !o->symtab || !o->strtab || !o->nsyms) return 0;
    for (uint64_t j = 0; j < o->nsyms; j++) {
        elf64_sym_t *s = &o->symtab[j];
        if (s->st_shndx == 0) continue;
        if (s->st_name >= o->strsz) continue;
        uint8_t type = s->st_info & 0xF;
        if (type != STT_OBJECT && type != STT_FUNC) continue;
        const char *n = o->strtab + s->st_name;
        if (strcmp(n, name) == 0) {
            return o->base + s->st_value;
        }
    }
    return 0;
}

static uint64_t resolve_symbol(const char *name) {
    uint64_t a = lookup_in(g_main, name);
    if (a) return a;
    for (int i = 0; i < g_nlibs; i++) {
        a = lookup_in(&g_libs[i], name);
        if (a) return a;
    }
    return 0;
}

static void apply_rela(loaded_obj_t *me, elf64_rela_t *rela, uint64_t bytes) {
    uint64_t count = bytes / sizeof(elf64_rela_t);
    for (uint64_t i = 0; i < count; i++) {
        uint32_t type = ELF64_R_TYPE(rela[i].r_info);
        uint32_t sidx = ELF64_R_SYM(rela[i].r_info);
        uint64_t *where = (uint64_t *)(uintptr_t)(me->base + rela[i].r_offset);

        if (type == R_X86_64_RELATIVE) {
            *where = me->base + (uint64_t)rela[i].r_addend;
            continue;
        }

        const char *name = 0;
        if (me->symtab && me->strtab && sidx < me->nsyms) {
            if (me->symtab[sidx].st_name < me->strsz) {
                name = me->strtab + me->symtab[sidx].st_name;
            }
        }

        switch (type) {
        case R_X86_64_GLOB_DAT:
        case R_X86_64_JUMP_SLOT: {
            if (!name) die("bogus symbol index");
            uint64_t addr = resolve_symbol(name);
            if (!addr) { puts("ld.so: undefined "); puts(name); die(""); }
            *where = addr;
            break;
        }
        case R_X86_64_64: {
            if (!name) break;
            uint64_t addr = resolve_symbol(name);
            *where = addr + (uint64_t)rela[i].r_addend;
            break;
        }
        case R_X86_64_PC32:
        case R_X86_64_PLT32: {
            if (!name) break;
            uint64_t addr = resolve_symbol(name);
            /* Truncated-to-32 write: gcc emits this for call/jmp
               displacements. Overflow is a link-time bug; we just
               write the low 32 bits. */
            int32_t disp = (int32_t)(addr + (uint64_t)rela[i].r_addend -
                                     (uint64_t)(uintptr_t)where);
            *(int32_t *)where = disp;
            break;
        }
        case R_X86_64_COPY: {
            if (!name) break;
            /* Find source in a library (skip main so we don't
               no-op copy from main's own slot). */
            uint64_t src = 0;
            for (int j = 0; j < g_nlibs; j++) {
                src = lookup_in(&g_libs[j], name);
                if (src) break;
            }
            if (!src) { puts("ld.so: COPY src "); puts(name); die(""); }
            uint64_t size = me->symtab[sidx].st_size;
            memcpy(where, (void *)(uintptr_t)src, (size_t)size);
            break;
        }
        case R_X86_64_NONE: break;
        default:
            puts("ld.so: unknown reloc type\n");
            break;
        }
    }
}

static int lib_already_loaded(const char *name) {
    for (int i = 0; i < g_nlibs; i++) {
        const char *p = g_libs[i].path;
        const char *base = p;
        for (; *p; p++) if (*p == '/') base = p + 1;
        if (strcmp(base, name) == 0) return 1;
    }
    return 0;
}

static int load_needed(loaded_obj_t *obj) {
    if (!obj->dyn || !obj->strtab) return 0;
    for (elf64_dyn_t *d = obj->dyn; d->d_tag != DT_NULL; d++) {
        if (d->d_tag != DT_NEEDED) continue;
        const char *name = obj->strtab + d->d_val;
        if (lib_already_loaded(name)) continue;
        if (g_nlibs >= MAX_LIBS) die("too many DT_NEEDED");

        char full[128];
        int j = 0;
        const char *pfx = "/lib/";
        while (pfx[j]) { full[j] = pfx[j]; j++; }
        int k = 0;
        while (name[k] && j < (int)sizeof(full) - 1) full[j++] = name[k++];
        full[j] = 0;

        int idx = g_nlibs;
        if (load_so(full, &g_libs[idx]) != 0) die("failed to load library");
        g_nlibs++;
        load_needed(&g_libs[idx]);
    }
    return 0;
}

static int init_main_obj(void *phdrs, uint32_t phnum, loaded_obj_t *out) {
    elf64_phdr_t *ph = phdrs;
    elf64_dyn_t *dyn = 0;
    for (uint32_t i = 0; i < phnum; i++) {
        if (ph[i].p_type == PT_DYNAMIC) {
            dyn = (elf64_dyn_t *)(uintptr_t)ph[i].p_vaddr;
            break;
        }
    }
    if (!dyn) return -1;
    out->base = 0; out->span = 0;
    out->path[0] = 'm'; out->path[1] = 'a'; out->path[2] = 'i';
    out->path[3] = 'n'; out->path[4] = 0;
    out->dyn = dyn;
    out->symtab = 0; out->strtab = 0; out->rela = 0; out->jmprela = 0;
    out->hash = 0; out->nsyms = 0; out->strsz = 0; out->relasz = 0;
    out->pltrelsz = 0;
    parse_dynamic(out);
    return 0;
}

/* --- dlopen / dlsym iface --------------------------------------- */

#define DL_IFACE_ADDR 0x50000000ULL

struct dl_ops {
    void *(*dlopen)(const char *, int);
    void *(*dlsym)(void *, const char *);
    int   (*dlclose)(void *);
    const char *(*dlerror)(void);
};

static const char *g_dl_err;

static void *dl_dlopen(const char *path, int flags) {
    (void)flags;
    if (!path) { g_dl_err = "dlopen: null path"; return 0; }

    const char *base = path;
    for (const char *p = path; *p; p++) if (*p == '/') base = p + 1;
    for (int i = 0; i < g_nlibs; i++) {
        const char *ob = g_libs[i].path;
        for (const char *p = g_libs[i].path; *p; p++) if (*p == '/') ob = p + 1;
        if (strcmp(ob, base) == 0) return &g_libs[i];
    }

    if (g_nlibs >= MAX_LIBS) { g_dl_err = "dlopen: too many libs"; return 0; }
    int start = g_nlibs;
    int idx = g_nlibs;
    if (load_so(path, &g_libs[idx]) != 0) { g_dl_err = "dlopen: load failed"; return 0; }
    g_nlibs++;
    load_needed(&g_libs[idx]);

    for (int i = start; i < g_nlibs; i++) {
        loaded_obj_t *o = &g_libs[i];
        if (o->rela    && o->relasz)   apply_rela(o, o->rela,    o->relasz);
        if (o->jmprela && o->pltrelsz) apply_rela(o, o->jmprela, o->pltrelsz);
    }
    g_dl_err = 0;
    return &g_libs[idx];
}

static void *dl_dlsym(void *handle, const char *name) {
    if (!name) { g_dl_err = "dlsym: null name"; return 0; }
    uint64_t a = handle ? lookup_in((loaded_obj_t *)handle, name)
                        : resolve_symbol(name);
    if (!a) { g_dl_err = "dlsym: not found"; return 0; }
    g_dl_err = 0;
    return (void *)(uintptr_t)a;
}

static int dl_dlclose(void *handle) { (void)handle; return 0; }

static const char *dl_dlerror(void) {
    const char *e = g_dl_err;
    g_dl_err = 0;
    return e;
}

static void install_dl_iface(void) {
    if (sys_mmap_anon((void *)DL_IFACE_ADDR, 4096,
                      PROT_READ | PROT_WRITE) != (long)DL_IFACE_ADDR) {
        puts("ld.so: dl iface mmap fail\n");
        return;
    }
    struct dl_ops *p = (struct dl_ops *)DL_IFACE_ADDR;
    p->dlopen  = dl_dlopen;
    p->dlsym   = dl_dlsym;
    p->dlclose = dl_dlclose;
    p->dlerror = dl_dlerror;
}

/* --- Entry ----------------------------------------------------- */

uint64_t ld_main(uint64_t *stack) {
    uint64_t at_phdr  = find_auxv(stack, AT_PHDR);
    uint64_t at_phnum = find_auxv(stack, AT_PHNUM);
    uint64_t at_entry = find_auxv(stack, AT_ENTRY);
    (void)find_auxv(stack, AT_BASE);

    if (!at_phdr || !at_phnum || !at_entry) die("missing auxv");

    static loaded_obj_t main_obj;
    if (init_main_obj((void *)(uintptr_t)at_phdr, (uint32_t)at_phnum, &main_obj) != 0) {
        die("main lacks PT_DYNAMIC");
    }
    g_main = &main_obj;

    if (!main_obj.dyn || !main_obj.strtab) die("main dyn incomplete");
    load_needed(&main_obj);

    for (int i = 0; i < g_nlibs; i++) {
        loaded_obj_t *o = &g_libs[i];
        if (o->rela    && o->relasz)   apply_rela(o, o->rela,    o->relasz);
        if (o->jmprela && o->pltrelsz) apply_rela(o, o->jmprela, o->pltrelsz);
    }
    if (main_obj.rela    && main_obj.relasz)   apply_rela(&main_obj, main_obj.rela,    main_obj.relasz);
    if (main_obj.jmprela && main_obj.pltrelsz) apply_rela(&main_obj, main_obj.jmprela, main_obj.pltrelsz);

    install_dl_iface();
    return at_entry;
}
