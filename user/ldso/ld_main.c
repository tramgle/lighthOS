/* ld-vibeos.so.1 — user-space dynamic linker.
 *
 * Loaded by the kernel at 0x40000000 alongside the main executable.
 * Given a SysV-style stack (argc, argv, envp, auxv), we:
 *
 *   1. Parse auxv to find main's program headers, entry, and our own
 *      load base (AT_BASE — unused once the design went ET_EXEC interp
 *      at a fixed address, but we honor it).
 *   2. Walk main's phdrs for PT_DYNAMIC.
 *   3. For each DT_NEEDED entry, open /lib/<name>, map its PT_LOADs
 *      via sys_mmap_anon at ascending bases (DYN_REGION_START+).
 *   4. Apply each library's own R_386_RELATIVE relocations so its
 *      .got is self-consistent.
 *   5. Apply main's R_386_GLOB_DAT and R_386_JMP_SLOT relocations,
 *      resolving symbols across the loaded libraries.
 *   6. Return main's entry to crt0_ldso, which restores ESP and jumps.
 *
 * Deferred (M4+): R_386_COPY, lazy binding, DT_RPATH, dlopen. */

#include "syscall.h"
#include "ulib.h"

/* Narrow integer types for the ELF structs below. syscall.h only
   provides uint32_t/int32_t; stdint.h would redefine them (harmless
   under C11 but noisy under -Wextra), so declare what we need locally. */
typedef unsigned char  uint8_t;
typedef unsigned short uint16_t;

/* ---- Minimal ELF32 definitions (subset of kernel/elf.h) ---- */

typedef struct {
    uint32_t e_ident_magic;
    uint8_t  e_ident_class, e_ident_data, e_ident_version, e_ident_osabi;
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
} elf32_ehdr_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} elf32_phdr_t;

typedef struct {
    int32_t  d_tag;
    uint32_t d_val;       /* also d_ptr; same storage */
} elf32_dyn_t;

typedef struct {
    uint32_t st_name;
    uint32_t st_value;
    uint32_t st_size;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
} elf32_sym_t;

typedef struct {
    uint32_t r_offset;
    uint32_t r_info;
} elf32_rel_t;

#define ELF_MAGIC 0x464C457F
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
#define DT_REL      17
#define DT_RELSZ    18
#define DT_RELENT   19
#define DT_PLTREL   20
#define DT_JMPREL   23

#define R_386_NONE      0
#define R_386_32        1
#define R_386_PC32      2
#define R_386_COPY      5
#define R_386_GLOB_DAT  6
#define R_386_JMP_SLOT  7
#define R_386_RELATIVE  8

#define AT_NULL  0
#define AT_PHDR  3
#define AT_PHENT 4
#define AT_PHNUM 5
#define AT_BASE  7
#define AT_ENTRY 9

/* ---- Configuration ---- */

/* Where to start loading shared libraries. First-fit ascending. */
#define DYN_REGION_START 0x30000000u
#define DYN_REGION_END   0x3FFFFFFFu
#define MAX_LIBS 8

/* ---- State ---- */

typedef struct {
    uint32_t     base;        /* load offset: runtime vaddr = base + link-time p_vaddr */
    uint32_t     span;        /* total bytes mapped */
    char         path[128];   /* persistent copy so duplicate-check
                                  survives load_needed()'s stack buffer */
    elf32_dyn_t *dyn;         /* runtime pointer to .dynamic */
    elf32_sym_t *symtab;
    const char  *strtab;
    uint32_t     strsz;
    uint32_t     nsyms;
    elf32_rel_t *rel;
    uint32_t     relsz;
    elf32_rel_t *jmprel;
    uint32_t     pltrelsz;
    uint32_t    *hash;        /* DT_HASH table (nbucket,nchain,buckets,chains) */
} loaded_obj_t;

static loaded_obj_t g_libs[MAX_LIBS];
static int          g_nlibs;
static uint32_t     g_dyn_next = DYN_REGION_START;
/* Main executable registered here so resolve_symbol can check it
   first and implement global-symbol interposition. Necessary for
   R_386_COPY semantics: library R_386_GLOB_DAT entries for a symbol
   main also defines (via the linker-generated copy slot) must
   resolve to main's address so library writes land in the shared
   slot. */
static loaded_obj_t *g_main;

/* Forward decl — used before definition. */
static int load_needed(loaded_obj_t *obj);

/* ---- Helpers ---- */

static void die(const char *msg) {
    puts("ld.so: ");
    puts(msg);
    puts("\n");
    sys_exit(127);
}

static uint32_t find_auxv(uint32_t *stack, uint32_t type) {
    /* stack[0]=argc, stack[1..argc]=argv, stack[argc+1]=NULL (argv end)
       then envp (pointers until NULL), then auxv pairs ending in AT_NULL. */
    uint32_t argc = stack[0];
    uint32_t *p = &stack[1 + argc + 1];   /* skip argv + NULL terminator */
    while (*p) p++;                        /* skip envp until NULL */
    p++;                                   /* past envp NULL — at auxv[0] */
    while (*p != AT_NULL) {
        if (*p == type) return p[1];
        p += 2;
    }
    return 0;
}

/* Walk a .dynamic section and populate the dt_* fields on `obj`.
   `dyn` pointers use d_val as both numeric and pointer storage — for
   pointer tags we add obj->base to the stored link-time address. */
static void parse_dynamic(loaded_obj_t *obj) {
    for (elf32_dyn_t *d = obj->dyn; d->d_tag != DT_NULL; d++) {
        uint32_t v = d->d_val;
        switch (d->d_tag) {
        case DT_SYMTAB:   obj->symtab  = (elf32_sym_t *)(obj->base + v); break;
        case DT_STRTAB:   obj->strtab  = (const char  *)(obj->base + v); break;
        case DT_STRSZ:    obj->strsz   = v; break;
        case DT_REL:      obj->rel     = (elf32_rel_t *)(obj->base + v); break;
        case DT_RELSZ:    obj->relsz   = v; break;
        case DT_JMPREL:   obj->jmprel  = (elf32_rel_t *)(obj->base + v); break;
        case DT_PLTRELSZ: obj->pltrelsz = v; break;
        case DT_HASH:     obj->hash    = (uint32_t *)(obj->base + v); break;
        default: break;
        }
    }
    /* Prefer DT_HASH[1] (nchain == nsyms) if present. Otherwise we'll
       fall back to scanning strtab indices in resolve_symbol. */
    if (obj->hash) obj->nsyms = obj->hash[1];
}

/* Round up to page size. */
static uint32_t page_up(uint32_t v) { return (v + 0xFFFu) & ~0xFFFu; }

/* Load a shared library from `path` into the caller's address space
   at the next free base. On success, fills `*out` and advances
   g_dyn_next. Returns 0 on success, -1 on failure. */
static int load_so(const char *path, loaded_obj_t *out) {
    int fd = sys_open(path, O_RDONLY);
    if (fd < 0) { puts("ld.so: cannot open "); puts(path); puts("\n"); return -1; }

    /* Read ELF header. */
    elf32_ehdr_t ehdr;
    if (sys_read(fd, &ehdr, sizeof ehdr) != sizeof ehdr) { sys_close(fd); return -1; }
    if (ehdr.e_ident_magic != ELF_MAGIC) { sys_close(fd); return -1; }
    if (ehdr.e_type != ET_DYN)           { sys_close(fd); return -1; }
    if (ehdr.e_phnum == 0 || ehdr.e_phnum > 16) { sys_close(fd); return -1; }

    /* Read program headers. */
    elf32_phdr_t phdrs[16];
    if (sys_lseek(fd, ehdr.e_phoff, 0) < 0) { sys_close(fd); return -1; }
    uint32_t phsize = (uint32_t)ehdr.e_phnum * sizeof(elf32_phdr_t);
    if (sys_read(fd, phdrs, phsize) != (int32_t)phsize) { sys_close(fd); return -1; }

    /* Compute total load span across all PT_LOADs. */
    uint32_t min_v = 0xFFFFFFFFu, max_v = 0;
    for (int i = 0; i < ehdr.e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;
        uint32_t s = phdrs[i].p_vaddr & ~0xFFFu;
        uint32_t e = page_up(phdrs[i].p_vaddr + phdrs[i].p_memsz);
        if (s < min_v) min_v = s;
        if (e > max_v) max_v = e;
    }
    if (min_v == 0xFFFFFFFFu) { sys_close(fd); return -1; }
    uint32_t span = max_v - min_v;

    /* Reserve the region. Advance g_dyn_next to the next megabyte so
       successive .so loads don't abut too tightly. */
    uint32_t region = g_dyn_next;
    if (region + span > DYN_REGION_END) { sys_close(fd); return -1; }
    if (sys_mmap_anon(region, span, PROT_READ | PROT_WRITE) != (int32_t)region) {
        sys_close(fd); return -1;
    }
    uint32_t base = region - min_v;   /* runtime = base + link-time p_vaddr */
    g_dyn_next = (region + span + 0xFFFFFu) & ~0xFFFFFu;  /* 1MB align */

    /* Read each PT_LOAD's filesz bytes into its runtime slot. BSS
       (memsz - filesz) is already zeroed by mmap_anon. */
    for (int i = 0; i < ehdr.e_phnum; i++) {
        if (phdrs[i].p_type != PT_LOAD) continue;
        uint32_t dest = base + phdrs[i].p_vaddr;
        if (phdrs[i].p_filesz > 0) {
            if (sys_lseek(fd, phdrs[i].p_offset, 0) < 0) { sys_close(fd); return -1; }
            if (sys_read(fd, (void *)dest, phdrs[i].p_filesz) !=
                (int32_t)phdrs[i].p_filesz) {
                sys_close(fd); return -1;
            }
        }
    }

    /* Find PT_DYNAMIC. */
    elf32_dyn_t *dyn = 0;
    for (int i = 0; i < ehdr.e_phnum; i++) {
        if (phdrs[i].p_type == PT_DYNAMIC) {
            dyn = (elf32_dyn_t *)(base + phdrs[i].p_vaddr);
            break;
        }
    }
    sys_close(fd);

    if (!dyn) return -1;

    out->base   = base;
    out->span   = span;
    /* Persist the path so duplicate detection (by basename compare)
       works after the caller's stack buffer goes away. */
    {
        int i = 0;
        while (path[i] && i < (int)sizeof(out->path) - 1) {
            out->path[i] = path[i]; i++;
        }
        out->path[i] = '\0';
    }
    out->dyn    = dyn;
    out->symtab = 0; out->strtab = 0; out->rel = 0; out->jmprel = 0;
    out->hash   = 0; out->nsyms = 0; out->strsz = 0; out->relsz = 0;
    out->pltrelsz = 0;
    parse_dynamic(out);
    return 0;
}

/* Look up a defined symbol in a single object. Returns 0 if not
   defined there. Used by resolve_symbol to implement main-first
   interposition.
   Skipping NOTYPE symbols is critical: GNU ld emits pseudo-symbols
   into main's .dynsym for taking-the-address-of imported functions,
   pointing at .got.plt slots (e.g. `setjmp` at the end of main's
   .got.plt, size 0, NOTYPE). Treating these as defined for
   interposition would make library call-through-PLT resolve to a
   data address, crashing on the indirect jump. Only actual FUNC
   and OBJECT definitions participate. */
#define STT_NOTYPE 0
#define STT_OBJECT 1
#define STT_FUNC   2

static uint32_t lookup_in(loaded_obj_t *o, const char *name) {
    if (!o || !o->symtab || !o->strtab || !o->nsyms) return 0;
    for (uint32_t j = 0; j < o->nsyms; j++) {
        elf32_sym_t *s = &o->symtab[j];
        if (s->st_shndx == 0) continue;          /* undefined */
        if (s->st_name >= o->strsz) continue;    /* garbage index */
        uint8_t type = s->st_info & 0xf;
        if (type != STT_OBJECT && type != STT_FUNC) continue;
        const char *n = o->strtab + s->st_name;
        if (strcmp(n, name) == 0) {
            return o->base + s->st_value;
        }
    }
    return 0;
}

/* Walk loaded objects looking for a defined symbol. Main executable
   is checked first (global interposition semantics — a library
   GLOB_DAT reloc for a symbol main also defines must resolve to
   main's address, e.g. R_386_COPY of `environ`). Libraries searched
   in load order. Linear scan — fine for our few hundred symbols. */
static uint32_t resolve_symbol(const char *name) {
    uint32_t a = lookup_in(g_main, name);
    if (a) return a;
    for (int i = 0; i < g_nlibs; i++) {
        a = lookup_in(&g_libs[i], name);
        if (a) return a;
    }
    return 0;
}

/* Apply a relocation table against `me`'s runtime addresses. `me` is
   the object being relocated (whose GOT entries we're fixing up);
   symbol lookup walks all loaded objects via resolve_symbol. */
static void apply_rel(loaded_obj_t *me, elf32_rel_t *rel, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        uint32_t type = rel[i].r_info & 0xFF;
        uint32_t sidx = rel[i].r_info >> 8;
        uint32_t *where = (uint32_t *)(me->base + rel[i].r_offset);

        if (type == R_386_RELATIVE) {
            /* B + A: original value is the addend. */
            *where += me->base;
            continue;
        }

        /* Symbol-referring relocs need to look up the symbol in
           the defining object. */
        const char *name = 0;
        if (me->symtab && me->strtab && sidx < me->nsyms) {
            if (me->symtab[sidx].st_name < me->strsz) {
                name = me->strtab + me->symtab[sidx].st_name;
            }
        }

        switch (type) {
        case R_386_GLOB_DAT:
        case R_386_JMP_SLOT: {
            if (!name) { puts("ld.so: bogus symbol index\n"); sys_exit(127); }
            uint32_t addr = resolve_symbol(name);
            if (!addr) {
                puts("ld.so: undefined symbol ");
                puts(name);
                puts("\n");
                sys_exit(127);
            }
            *where = addr;
            break;
        }
        case R_386_32: {
            if (!name) break;
            uint32_t addr = resolve_symbol(name);
            *where = addr + *where;  /* addend is at *where */
            break;
        }
        case R_386_PC32: {
            if (!name) break;
            uint32_t addr = resolve_symbol(name);
            *where = addr + *where - (uint32_t)where;
            break;
        }
        case R_386_COPY: {
            /* Main exec has a BSS slot reserved for a variable that
               lives in a shared library. Copy the library's initial
               value into main's slot. We deliberately SKIP g_main
               during this lookup — resolve_symbol's interposition
               semantics would return main's own slot (because main's
               .dynsym lists this symbol as defined at its BSS slot
               address), making the memcpy a no-op. For the copy
               source we want the actual library definition. Size
               comes from main's symbol entry. */
            if (!name) break;
            uint32_t src = 0;
            for (int i = 0; i < g_nlibs; i++) {
                src = lookup_in(&g_libs[i], name);
                if (src) break;
            }
            if (!src) {
                puts("ld.so: R_386_COPY undefined ");
                puts(name);
                puts("\n");
                sys_exit(127);
            }
            uint32_t size = me->symtab[sidx].st_size;
            memcpy(where, (void *)src, size);
            break;
        }
        case R_386_NONE:
            break;
        default:
            puts("ld.so: unknown reloc type\n");
            break;
        }
    }
}

/* True if a library with matching basename has already been loaded.
   DT_NEEDED stores the soname (e.g. "libulib.so.1"); compare that
   against the basename of the full path we stored on load. */
static int lib_already_loaded(const char *name) {
    for (int i = 0; i < g_nlibs; i++) {
        const char *p = g_libs[i].path;
        const char *base = p;
        for (; *p; p++) if (*p == '/') base = p + 1;
        if (strcmp(base, name) == 0) return 1;
    }
    return 0;
}

/* Walk `obj`'s DT_NEEDED entries and load each missing library into
   the dyn region. Recurses so libvibc.so.1's own DT_NEEDED (e.g.
   libulib.so.1) gets pulled in transitively. Returns 0 on success,
   die()s on failure. */
static int load_needed(loaded_obj_t *obj) {
    if (!obj->dyn || !obj->strtab) return 0;
    for (elf32_dyn_t *d = obj->dyn; d->d_tag != DT_NULL; d++) {
        if (d->d_tag != DT_NEEDED) continue;
        const char *name = obj->strtab + d->d_val;
        if (lib_already_loaded(name)) continue;
        if (g_nlibs >= MAX_LIBS) die("too many DT_NEEDED");

        /* Build /lib/<name> on the stack. load_so snapshots it into
           g_libs[idx].path so the pointer doesn't escape. */
        char full[128];
        int j = 0;
        const char *pfx = "/lib/";
        while (pfx[j]) { full[j] = pfx[j]; j++; }
        int k = 0;
        while (name[k] && j < (int)sizeof(full) - 1) full[j++] = name[k++];
        full[j] = '\0';

        int idx = g_nlibs;
        if (load_so(full, &g_libs[idx]) != 0) die("failed to load library");
        g_nlibs++;
        /* Recurse: this library's DT_NEEDEDs are our DT_NEEDEDs too. */
        load_needed(&g_libs[idx]);
    }
    return 0;
}

/* Build a loaded_obj_t for the main executable so its symbol table
   is searchable and its relocations can be applied uniformly.
   Main is ET_EXEC, base = 0. */
static int init_main_obj(uint32_t *phdrs, uint32_t phnum, loaded_obj_t *out) {
    elf32_phdr_t *ph = (elf32_phdr_t *)phdrs;
    elf32_dyn_t *dyn = 0;
    for (uint32_t i = 0; i < phnum; i++) {
        if (ph[i].p_type == PT_DYNAMIC) {
            dyn = (elf32_dyn_t *)ph[i].p_vaddr;  /* base=0 for ET_EXEC */
            break;
        }
    }
    if (!dyn) return -1;
    out->base = 0;
    out->span = 0;
    out->path[0] = 'm'; out->path[1] = 'a'; out->path[2] = 'i';
    out->path[3] = 'n'; out->path[4] = '\0';
    out->dyn = dyn;
    out->symtab = 0; out->strtab = 0; out->rel = 0; out->jmprel = 0;
    out->hash = 0; out->nsyms = 0; out->strsz = 0; out->relsz = 0;
    out->pltrelsz = 0;
    parse_dynamic(out);
    return 0;
}

/* ---- dlopen / dlsym ----
   Exposed to user code via a function-pointer table at DL_IFACE_ADDR
   (see libulib's dlopen shim). ld.so publishes the table during
   ld_main startup via sys_mmap_anon + pointer fill. We deliberately
   keep this surface small: RTLD_* flags are parsed only for future
   compatibility; we always resolve eagerly and globally. */

#define DL_IFACE_ADDR 0x50000000u

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

    /* Basename for already-loaded check. */
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
    if (load_so(path, &g_libs[idx]) != 0) {
        g_dl_err = "dlopen: load failed";
        return 0;
    }
    g_nlibs++;
    /* Pull in the new lib's DT_NEEDED recursively. */
    load_needed(&g_libs[idx]);

    /* Relocate everything we just loaded (main's symbol table is
       checked first via resolve_symbol → interposition semantics). */
    for (int i = start; i < g_nlibs; i++) {
        loaded_obj_t *o = &g_libs[i];
        if (o->rel && o->relsz)
            apply_rel(o, o->rel, o->relsz / sizeof(elf32_rel_t));
        if (o->jmprel && o->pltrelsz)
            apply_rel(o, o->jmprel, o->pltrelsz / sizeof(elf32_rel_t));
    }

    g_dl_err = 0;
    return &g_libs[idx];
}

static void *dl_dlsym(void *handle, const char *name) {
    if (!name) { g_dl_err = "dlsym: null name"; return 0; }
    uint32_t a;
    if (!handle) {
        a = resolve_symbol(name);
    } else {
        a = lookup_in((loaded_obj_t *)handle, name);
    }
    if (!a) { g_dl_err = "dlsym: not found"; return 0; }
    g_dl_err = 0;
    return (void *)a;
}

static int dl_dlclose(void *handle) {
    (void)handle;
    /* No refcounting yet — libraries stay resident until the process
       exits. dlclose is a no-op to match API expectations. */
    return 0;
}

static const char *dl_dlerror(void) {
    const char *e = g_dl_err;
    g_dl_err = 0;  /* dlerror consumes the error */
    return e;
}

/* Install the dl_ops table at DL_IFACE_ADDR so libulib's dlopen
   wrappers can reach us. Called once from ld_main after the main
   exec's DT_NEEDED libraries are relocated. */
static void install_dl_iface(void) {
    if (sys_mmap_anon(DL_IFACE_ADDR, 4096,
                      PROT_READ | PROT_WRITE) != (int32_t)DL_IFACE_ADDR) {
        puts("ld.so: failed to map dl interface page\n");
        return;
    }
    struct dl_ops *p = (struct dl_ops *)DL_IFACE_ADDR;
    p->dlopen  = dl_dlopen;
    p->dlsym   = dl_dlsym;
    p->dlclose = dl_dlclose;
    p->dlerror = dl_dlerror;
    /* Leave the page RW so ld.so can still touch it; dropping to RO
       would require tracking that we need to flip back for error-
       string writes. */
}

/* ---- Entry ---- */

uint32_t ld_main(uint32_t *stack) {
    /* Pull out the auxv fields the kernel handed us. */
    uint32_t at_phdr  = find_auxv(stack, AT_PHDR);
    uint32_t at_phnum = find_auxv(stack, AT_PHNUM);
    uint32_t at_entry = find_auxv(stack, AT_ENTRY);
    (void)find_auxv(stack, AT_BASE);  /* AT_BASE unused for now */

    if (!at_phdr || !at_phnum || !at_entry) die("missing auxv");

    /* Set up main's loaded_obj_t. Register globally so resolve_symbol
       can interpose main's definitions over library ones. */
    static loaded_obj_t main_obj;  /* static so g_main survives ld_main return */
    if (init_main_obj((uint32_t *)at_phdr, at_phnum, &main_obj) != 0) {
        die("main lacks PT_DYNAMIC");
    }
    g_main = &main_obj;

    /* Walk main's DT_NEEDED (and each dependent's DT_NEEDED,
       transitively). Libraries are loaded into the dyn region at
       ascending bases. Must complete before applying any relocations
       so every candidate defining object is findable. */
    if (!main_obj.dyn || !main_obj.strtab) die("main dyn incomplete");
    load_needed(&main_obj);

    /* Apply each loaded library's own R_386_RELATIVE entries (its GOT
       needs base applied). R_386_GLOB_DAT/JMP_SLOT in the library's
       tables reference library-internal symbols — resolve_symbol
       finds them within the same object via the linear walk. */
    for (int i = 0; i < g_nlibs; i++) {
        loaded_obj_t *o = &g_libs[i];
        if (o->rel && o->relsz) {
            apply_rel(o, o->rel, o->relsz / sizeof(elf32_rel_t));
        }
        if (o->jmprel && o->pltrelsz) {
            apply_rel(o, o->jmprel, o->pltrelsz / sizeof(elf32_rel_t));
        }
    }

    /* Apply main's relocations — these reference symbols in the
       loaded libraries. */
    if (main_obj.rel && main_obj.relsz) {
        apply_rel(&main_obj, main_obj.rel, main_obj.relsz / sizeof(elf32_rel_t));
    }
    if (main_obj.jmprel && main_obj.pltrelsz) {
        apply_rel(&main_obj, main_obj.jmprel, main_obj.pltrelsz / sizeof(elf32_rel_t));
    }

    /* Publish the dlopen/dlsym table before jumping to main so the
       process can load shared objects at runtime. */
    install_dl_iface();

    return at_entry;
}
