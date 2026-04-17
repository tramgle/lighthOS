/* Minimal x86_64 process layer.
 *
 * Pares the i386 process.c down to just what's needed to run real
 * user binaries + fork/wait/exec + console I/O:
 *   - process_t array (no linked lists; fixed MAX=8)
 *   - process_spawn_from_memory: build a PML4, load ELF, stack,
 *     argv/envp/auxv, schedule as a new task that iretq's to ring 3
 *   - process_fork: snapshot parent user pages, duplicate fd table
 *   - process_waitpid: block until child exits, reap status
 *   - process_exit: mark dead, yield
 *
 * Signals, pgid, chroot, pipes, strace, alarm — all deferred.
 * Future sessions will bring them back file-by-file.
 */

#include "kernel/process.h"
#include "kernel/elf.h"
#include "kernel/task.h"
#include "kernel/tss.h"
#include "kernel/gdt.h"
#include "kernel/panic.h"
#include "mm/heap.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "lib/string.h"
#include "lib/kprintf.h"
#include "drivers/serial.h"
#include "fs/vfs.h"
#include "kernel/pipe.h"

#define USER_STACK_TOP 0x0000000000800000ULL
#define USER_STACK_PAGES 4

static process_t processes[PROCESS_MAX];
static uint32_t next_pid = 1;

static void fd_drop_ref(fd_entry_t *e);

void process_init(void) {
    memset(processes, 0, sizeof(processes));
}

/* Drivers (serial.c) reach into process state for Ctrl-C / Ctrl-Z
   routing. Weak stubs until the foreground-pgid layer is re-ported. */
void process_kill_foreground(void) __attribute__((weak));
void process_kill_foreground(void) { }
void process_stop_foreground(void) __attribute__((weak));
void process_stop_foreground(void) { }

process_t *process_current(void) {
    task_t *t = task_current();
    if (!t) return 0;
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (processes[i].alive && processes[i].task == t) return &processes[i];
    }
    return 0;
}

process_t *process_get(uint32_t pid) {
    for (int i = 0; i < PROCESS_MAX; i++)
        if (processes[i].alive && processes[i].pid == pid) return &processes[i];
    return 0;
}

process_t *process_alloc(const char *name) {
    /* Reap any already-waitpid'd slots first. */
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (!processes[i].alive && processes[i].reaped) {
            memset(&processes[i], 0, sizeof(processes[i]));
        }
    }
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (!processes[i].alive && processes[i].pid == 0) {
            process_t *p = &processes[i];
            memset(p, 0, sizeof(*p));
            p->pid = next_pid++;
            strncpy(p->name, name, sizeof(p->name) - 1);
            p->alive = true;
            p->fds[0].type = FD_CONSOLE;
            p->fds[1].type = FD_CONSOLE;
            p->fds[2].type = FD_CONSOLE;
            /* Default root = "/", cwd = "/" — inherited via fork. */
            p->root[0] = '/'; p->root[1] = 0;
            p->cwd[0] = '/';  p->cwd[1] = 0;
            return p;
        }
    }
    return 0;
}

/* Canonicalize `path` against process cwd+root, writing the host-
   absolute result to `out`. Handles "." / ".." / double-slash,
   clamps ".." at the chroot boundary. */
static int canon_into(const char *path, const char *cwd,
                      char *out, int cap) {
    char tmp[VFS_MAX_PATH];
    int ti = 0;
    if (path[0] == '/') {
        tmp[ti++] = '/';
        path++;
    } else {
        for (int i = 0; cwd[i] && ti < (int)sizeof(tmp) - 1; i++) tmp[ti++] = cwd[i];
        if (ti == 0) tmp[ti++] = '/';
        if (tmp[ti-1] != '/' && ti < (int)sizeof(tmp) - 1) tmp[ti++] = '/';
    }
    for (int i = 0; path[i]; i++) {
        if (ti >= (int)sizeof(tmp) - 1) return -1;
        tmp[ti++] = path[i];
    }
    tmp[ti] = 0;

    /* Now canonicalize in-place into `out`. */
    int oi = 0;
    out[oi++] = '/';
    int si = 0;
    if (tmp[0] == '/') si = 1;
    while (tmp[si]) {
        /* collapse // */
        if (tmp[si] == '/') { si++; continue; }
        /* find next slash */
        int seg = si;
        while (tmp[si] && tmp[si] != '/') si++;
        int seglen = si - seg;
        if (seglen == 1 && tmp[seg] == '.') continue;
        if (seglen == 2 && tmp[seg] == '.' && tmp[seg+1] == '.') {
            /* walk back one segment in out (stay at root) */
            if (oi > 1) {
                oi--;                        /* drop trailing '/' */
                while (oi > 1 && out[oi-1] != '/') oi--;
            }
            continue;
        }
        if (oi + seglen + 1 >= cap) return -1;
        for (int k = 0; k < seglen; k++) out[oi++] = tmp[seg + k];
        out[oi++] = '/';
    }
    /* Strip trailing slash except at root. */
    if (oi > 1 && out[oi-1] == '/') oi--;
    out[oi] = 0;
    return 0;
}

int process_resolve_path(const char *path, char *out, int cap) {
    process_t *p = process_current();
    const char *cwd  = (p && p->cwd[0])  ? p->cwd  : "/";
    const char *root = (p && p->root[0]) ? p->root : "/";
    char local[VFS_MAX_PATH];
    if (canon_into(path, cwd, local, (int)sizeof(local)) != 0) return -1;
    /* Prepend root. Special-case root == "/" (no prefix). */
    if (root[0] == '/' && root[1] == 0) {
        int n = 0;
        while (local[n] && n < cap - 1) { out[n] = local[n]; n++; }
        out[n] = 0;
        return 0;
    }
    int oi = 0;
    for (int i = 0; root[i] && oi < cap - 1; i++) out[oi++] = root[i];
    if (oi > 0 && out[oi-1] == '/') oi--;
    for (int i = 0; local[i] && oi < cap - 1; i++) out[oi++] = local[i];
    out[oi] = 0;
    return 0;
}

/* --- User stack layout (SysV AMD64) --------------------------- */
/* The ABI requires 16-byte-aligned RSP *before* the first user
   instruction. We push argv pointers + strings + NULL envp + NULL
   auxv from the top down and land with RSP % 16 == 0. */
#define SPAWN_ENVP_MAX 32

/* Write a single byte into the target PML4 via its HHDM-backed
   frame. Returns 0 on missing mapping. */
static int stack_put_byte(uint64_t *pml4, uint64_t va, uint8_t b) {
    uint64_t page = va & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t phys = vmm_get_physical_in(pml4, page);
    if (!phys) return 0;
    ((uint8_t *)phys_to_virt_low(phys))[va - page] = b;
    return 1;
}
static int stack_put_u64(uint64_t *pml4, uint64_t va, uint64_t v) {
    uint64_t page = va & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t phys = vmm_get_physical_in(pml4, page);
    if (!phys) return 0;
    *(uint64_t *)((uint8_t *)phys_to_virt_low(phys) + (va - page)) = v;
    return 1;
}

static uint64_t build_user_stack(uint64_t *pml4, process_t *p,
                                 int argc, const char **argv_src,
                                 int envc, const char **envp_src) {
    (void)p;
    uint64_t sp = USER_STACK_TOP;

    /* Push strings first, top-down: envp, then argv. Record VAs. */
    uint64_t env_ptrs[SPAWN_ENVP_MAX];
    uint64_t arg_ptrs[SPAWN_ARGV_MAX];
    for (int i = envc - 1; i >= 0; i--) {
        uint64_t n = strlen(envp_src[i]) + 1;
        sp -= n;
        for (uint64_t o = 0; o < n; o++)
            if (!stack_put_byte(pml4, sp + o, (uint8_t)envp_src[i][o])) return 0;
        env_ptrs[i] = sp;
    }
    for (int i = argc - 1; i >= 0; i--) {
        uint64_t n = strlen(argv_src[i]) + 1;
        sp -= n;
        for (uint64_t o = 0; o < n; o++)
            if (!stack_put_byte(pml4, sp + o, (uint8_t)argv_src[i][o])) return 0;
        arg_ptrs[i] = sp;
    }

    /* Reserve vector slots: argc, argv[0..argc], NULL, envp[0..envc], NULL,
       auxv NULL pair. Pre-compute total and align so RSP%16==0 at entry. */
    sp &= ~(uint64_t)0xF;
    int total_slots = 1                 /* argc   */
                    + (argc + 1)        /* argv + NULL */
                    + (envc + 1)        /* envp + NULL */
                    + 2;                /* auxv AT_NULL */
    uint64_t need = (uint64_t)total_slots * 8;
    uint64_t wp = sp - need;
    wp &= ~(uint64_t)0xF;               /* entry alignment */
    sp = wp;

    if (!stack_put_u64(pml4, wp, (uint64_t)argc)) return 0; wp += 8;
    for (int i = 0; i < argc; i++) { if (!stack_put_u64(pml4, wp, arg_ptrs[i])) return 0; wp += 8; }
    if (!stack_put_u64(pml4, wp, 0)) return 0; wp += 8;       /* argv NULL */
    for (int i = 0; i < envc; i++) { if (!stack_put_u64(pml4, wp, env_ptrs[i])) return 0; wp += 8; }
    if (!stack_put_u64(pml4, wp, 0)) return 0; wp += 8;       /* envp NULL */
    if (!stack_put_u64(pml4, wp, 0)) return 0; wp += 8;       /* auxv key  */
    if (!stack_put_u64(pml4, wp, 0)) return 0; wp += 8;       /* auxv val  */

    return sp;
}

/* --- Spawn trampoline ----------------------------------------- */
/* Runs on the fresh kernel stack of the new task after schedule()
   picks it. Reads spawn_* fields from the process_t, iretq's to
   ring 3. */
static process_t *g_spawning;   /* passed from spawn-setup to trampoline */

static void spawn_trampoline(void) {
    process_t *p = g_spawning;
    g_spawning = 0;
    extern void jump_to_usermode(uint64_t entry, uint64_t rsp);
    /* Align stack pointer to 16 for SysV entry. Our build_user_stack
       already lands on a 16-aligned sp. */
    tss_set_kernel_stack(p->task->stack_base + TASK_STACK_SIZE);
    jump_to_usermode(p->spawn_entry, p->spawn_rsp);
}

int process_spawn_from_memory(const char *name, const void *elf,
                              uint64_t size, char *const argv[]) {
    process_t *p = process_alloc(name);
    if (!p) return -1;

    p->parent_pid = process_current() ? process_current()->pid : 0;

    uint64_t *pml4 = vmm_new_pml4();
    if (!pml4) { p->alive = false; return -1; }

    uint64_t entry = elf_load(elf, size, pml4);
    if (!entry) { vmm_free_pml4(pml4); p->alive = false; return -1; }

    /* User stack. */
    for (int i = 0; i < USER_STACK_PAGES; i++) {
        uint64_t va = USER_STACK_TOP - (i + 1) * PAGE_SIZE;
        uint64_t frame = pmm_alloc_frame();
        if (!frame) { vmm_free_pml4(pml4); p->alive = false; return -1; }
        memset(phys_to_virt_low(frame), 0, PAGE_SIZE);
        vmm_map_in(pml4, va, frame, VMM_FLAG_WRITE | VMM_FLAG_USER);
    }

    int argc = 0;
    const char *argv_local[SPAWN_ARGV_MAX];
    if (argv) {
        while (argv[argc] && argc < SPAWN_ARGV_MAX - 1) {
            argv_local[argc] = argv[argc]; argc++;
        }
    } else {
        argv_local[0] = name; argc = 1;
    }
    argv_local[argc] = 0;

    uint64_t rsp = build_user_stack(pml4, p, argc, argv_local, 0, 0);
    if (!rsp) { vmm_free_pml4(pml4); p->alive = false; return -1; }

    task_t *t = task_alloc(name);
    if (!t) { vmm_free_pml4(pml4); p->alive = false; return -1; }
    t->pml4 = pml4;
    task_set_entry(t, spawn_trampoline);
    p->task = t;
    p->spawn_entry = entry;
    p->spawn_rsp   = rsp;

    g_spawning = p;
    task_insert_ready(t);
    return p->pid;
}

int process_spawn_from_path(const char *path, char *const argv[]) {
    struct vfs_stat st;
    if (vfs_stat(path, &st) != 0 || st.type != VFS_FILE) return -1;
    if (st.size == 0) return -1;

    uint8_t *buf = kmalloc(st.size);
    if (!buf) return -1;
    ssize_t r = vfs_read(path, buf, st.size, 0);
    if (r != (ssize_t)st.size) { kfree(buf); return -1; }

    /* Derive a short name from the last path component. */
    const char *name = path;
    for (const char *p = path; *p; p++) if (*p == '/') name = p + 1;

    int pid = process_spawn_from_memory(name, buf, st.size, argv);
    kfree(buf);
    return pid;
}

int process_waitpid(uint32_t pid, int *status) {
    for (;;) {
        process_t *p = 0;
        for (int i = 0; i < PROCESS_MAX; i++) {
            if (processes[i].pid == pid) { p = &processes[i]; break; }
        }
        if (!p) return -1;
        if (!p->alive) {
            if (status) *status = p->exit_code;
            p->reaped = true;
            return (int)pid;
        }
        task_yield();
    }
}

void process_exit(int code) {
    process_t *p = process_current();
    if (p) {
        /* Release pipe refs so EOF reaches peers promptly; file
           fds have no ref, console stays. */
        for (int i = 0; i < FD_MAX; i++) {
            if (p->fds[i].type == FD_PIPE_READ ||
                p->fds[i].type == FD_PIPE_WRITE) {
                fd_drop_ref(&p->fds[i]);
            }
        }
        p->exit_code = code;
        p->alive = false;
    }
    if (p && p->task) p->task->state = TASK_DEAD;
    task_yield();
    for (;;) __asm__ volatile ("hlt");
}

/* Walk parent's PML4[0..255] and duplicate every user 4 KiB page
   into fresh child frames. Kernel half is already shared via
   vmm_new_pml4 so we don't touch PML4[256..511]. Returns 0 on OK,
   -1 on OOM. */
static int duplicate_user_space(uint64_t *parent_pml4, uint64_t *child_pml4) {
    for (uint32_t i4 = 0; i4 < 256; i4++) {
        uint64_t e4 = parent_pml4[i4];
        if (!(e4 & VMM_FLAG_PRESENT)) continue;
        uint64_t *p_pdpt = (uint64_t *)phys_to_virt_low(e4 & PTE_ADDR_MASK);
        for (uint32_t i3 = 0; i3 < 512; i3++) {
            uint64_t e3 = p_pdpt[i3];
            if (!(e3 & VMM_FLAG_PRESENT) || (e3 & VMM_FLAG_HUGE)) continue;
            uint64_t *p_pd = (uint64_t *)phys_to_virt_low(e3 & PTE_ADDR_MASK);
            for (uint32_t i2 = 0; i2 < 512; i2++) {
                uint64_t e2 = p_pd[i2];
                if (!(e2 & VMM_FLAG_PRESENT) || (e2 & VMM_FLAG_HUGE)) continue;
                uint64_t *p_pt = (uint64_t *)phys_to_virt_low(e2 & PTE_ADDR_MASK);
                for (uint32_t i1 = 0; i1 < 512; i1++) {
                    uint64_t e1 = p_pt[i1];
                    if (!(e1 & VMM_FLAG_PRESENT)) continue;
                    uint64_t va = ((uint64_t)i4 << 39) | ((uint64_t)i3 << 30) |
                                  ((uint64_t)i2 << 21) | ((uint64_t)i1 << 12);
                    uint64_t src_phys = e1 & PTE_ADDR_MASK;
                    uint64_t dst_phys = pmm_alloc_frame();
                    if (!dst_phys) return -1;
                    memcpy(phys_to_virt_low(dst_phys),
                           phys_to_virt_low(src_phys), PAGE_SIZE);
                    vmm_map_in(child_pml4, va, dst_phys, e1 & 0xFFF);
                }
            }
        }
    }
    return 0;
}

int process_fork(registers_t *parent_regs) {
    process_t *parent = process_current();
    if (!parent) return -1;

    process_t *child = process_alloc(parent->name);
    if (!child) return -1;
    child->parent_pid = parent->pid;
    /* Inherit chroot + cwd. */
    for (int i = 0; i < VFS_MAX_PATH; i++) child->root[i] = parent->root[i];
    for (int i = 0; i < VFS_MAX_PATH; i++) child->cwd[i]  = parent->cwd[i];

    for (int i = 0; i < FD_MAX; i++) {
        child->fds[i] = parent->fds[i];
        /* Each inherited pipe endpoint is a fresh reference to the
           same kernel pipe_t — bump reader/writer refcount so ref
           accounting reflects both parent and child fds. */
        if (child->fds[i].type == FD_PIPE_READ)
            pipe_add_reader((pipe_t *)child->fds[i].pipe);
        else if (child->fds[i].type == FD_PIPE_WRITE)
            pipe_add_writer((pipe_t *)child->fds[i].pipe);
    }

    uint64_t *child_pml4 = vmm_new_pml4();
    if (!child_pml4) { child->alive = false; return -1; }
    if (duplicate_user_space(parent->task->pml4, child_pml4) < 0) {
        vmm_free_pml4(child_pml4);
        child->alive = false;
        return -1;
    }

    task_t *t = task_alloc(child->name);
    if (!t) { vmm_free_pml4(child_pml4); child->alive = false; return -1; }
    t->pml4 = child_pml4;

    /* Plant a registers_t at the top of the child's kernel stack
       that mirrors the parent's interrupted frame, with RAX=0 so
       fork() returns 0 in the child. schedule() will pick up this
       task, restore CR3/TSS, and iretq back to ring 3 at the
       parent's saved RIP on the child's user stack. */
    uint64_t stack_top = t->stack_base + TASK_STACK_SIZE;
    registers_t *cf = (registers_t *)(uintptr_t)(stack_top - sizeof(registers_t));
    *cf = *parent_regs;
    cf->rax = 0;
    t->rsp = (uint64_t)(uintptr_t)cf;

    child->task = t;
    task_insert_ready(t);
    return (int)child->pid;
}

int process_execve_from_memory(registers_t *regs, const char *name,
                               const void *elf, uint64_t size,
                               char *const argv[], char *const envp[]) {
    process_t *p = process_current();
    if (!p) return -1;

    if (elf_validate(elf, size) != 0) return -1;

    /* Snapshot argv + envp into the process's spawn buffer so the
       pointers survive vmm_unmap_user_space. */
    const char *argv_local[SPAWN_ARGV_MAX];
    const char *envp_local[SPAWN_ENVP_MAX];
    int argc = 0, envc = 0;
    char *buf = p->spawn_argv_buf;
    uint64_t buf_used = 0;
    if (argv) {
        while (argv[argc] && argc < SPAWN_ARGV_MAX - 1) {
            uint64_t len = 0; while (argv[argc][len]) len++; len++;
            if (buf_used + len > SPAWN_ARGV_BUF) return -1;
            memcpy(buf + buf_used, argv[argc], (uint32_t)len);
            argv_local[argc] = buf + buf_used;
            buf_used += len; argc++;
        }
    } else {
        argv_local[0] = name ? name : ""; argc = 1;
    }
    argv_local[argc] = 0;
    if (envp) {
        while (envp[envc] && envc < SPAWN_ENVP_MAX - 1) {
            uint64_t len = 0; while (envp[envc][len]) len++; len++;
            if (buf_used + len > SPAWN_ARGV_BUF) return -1;
            memcpy(buf + buf_used, envp[envc], (uint32_t)len);
            envp_local[envc] = buf + buf_used;
            buf_used += len; envc++;
        }
    }
    envp_local[envc] = 0;

    uint64_t *pml4 = p->task->pml4;
    vmm_unmap_user_space(pml4);

    uint64_t entry = elf_load(elf, size, pml4);
    if (!entry) return -1;

    for (int i = 0; i < USER_STACK_PAGES; i++) {
        uint64_t va = USER_STACK_TOP - (i + 1) * PAGE_SIZE;
        uint64_t frame = pmm_alloc_frame();
        if (!frame) return -1;
        memset(phys_to_virt_low(frame), 0, PAGE_SIZE);
        vmm_map_in(pml4, va, frame, VMM_FLAG_WRITE | VMM_FLAG_USER);
    }

    uint64_t rsp = build_user_stack(pml4, p, argc, argv_local, envc, envp_local);
    if (!rsp) return -1;

    strncpy(p->name, name ? name : "exec", sizeof(p->name) - 1);

    /* Rewrite the user-register frame so syscall return iretq's
       into the new image. Clean SysV AMD64 entry state. */
    regs->rip = entry;
    regs->rsp = rsp;
    regs->rflags = 0x202;                    /* IF=1 */
    regs->cs = 0x1B;
    regs->ss = 0x23;
    regs->rax = regs->rbx = regs->rcx = regs->rdx = 0;
    regs->rsi = regs->rdi = regs->rbp = 0;
    regs->r8 = regs->r9 = regs->r10 = regs->r11 = 0;
    regs->r12 = regs->r13 = regs->r14 = regs->r15 = 0;

    return 0;
}

/* --- File descriptor plumbing -------------------------------- */

static fd_entry_t *current_fds(void) {
    process_t *p = process_current();
    return p ? p->fds : 0;
}

int fd_open(const char *path, uint32_t flags) {
    fd_entry_t *fds = current_fds();
    if (!fds) return -1;

    struct vfs_stat st;
    int exists = vfs_stat(path, &st) == 0;
    if (!exists) {
        if (!(flags & O_CREAT)) return -1;
        if (vfs_create(path, VFS_FILE) != 0) return -1;
        if (vfs_stat(path, &st) != 0) return -1;
    } else if (flags & O_TRUNC) {
        /* Truncate by recreating the file. Simplest semantics given
           the ramfs interface we have. */
        vfs_unlink(path);
        if (vfs_create(path, VFS_FILE) != 0) return -1;
        if (vfs_stat(path, &st) != 0) return -1;
    }

    for (int i = 3; i < FD_MAX; i++) {
        if (fds[i].type == FD_NONE) {
            fds[i].type   = FD_FILE;
            strncpy(fds[i].path, path, VFS_MAX_PATH - 1);
            fds[i].path[VFS_MAX_PATH - 1] = 0;
            fds[i].offset = (flags & O_APPEND) ? st.size : 0;
            fds[i].flags  = flags;
            return i;
        }
    }
    return -1;
}

static void fd_drop_ref(fd_entry_t *e) {
    if (e->type == FD_PIPE_READ)  pipe_close_reader((pipe_t *)e->pipe);
    if (e->type == FD_PIPE_WRITE) pipe_close_writer((pipe_t *)e->pipe);
    e->type = FD_NONE;
    e->path[0] = 0;
    e->offset = 0;
    e->flags  = 0;
    e->pipe   = 0;
}

int fd_close(int fd) {
    fd_entry_t *fds = current_fds();
    if (!fds || fd < 0 || fd >= FD_MAX) return -1;
    if (fds[fd].type == FD_NONE) return -1;
    if (fds[fd].type == FD_CONSOLE) return 0;
    fd_drop_ref(&fds[fd]);
    return 0;
}

int fd_pipe(int fds_out[2]) {
    fd_entry_t *fds = current_fds();
    if (!fds) return -1;
    int rfd = -1, wfd = -1;
    for (int i = 3; i < FD_MAX; i++) {
        if (fds[i].type == FD_NONE) {
            if (rfd < 0) rfd = i;
            else if (wfd < 0) { wfd = i; break; }
        }
    }
    if (rfd < 0 || wfd < 0) return -1;
    pipe_t *p = pipe_create();
    if (!p) return -1;
    pipe_add_reader(p);
    pipe_add_writer(p);
    fds[rfd].type = FD_PIPE_READ;  fds[rfd].pipe = p;
    fds[wfd].type = FD_PIPE_WRITE; fds[wfd].pipe = p;
    fds_out[0] = rfd; fds_out[1] = wfd;
    return 0;
}

int fd_dup2(int oldfd, int newfd) {
    fd_entry_t *fds = current_fds();
    if (!fds) return -1;
    if (oldfd < 0 || oldfd >= FD_MAX) return -1;
    if (newfd < 0 || newfd >= FD_MAX) return -1;
    if (fds[oldfd].type == FD_NONE) return -1;
    if (oldfd == newfd) return newfd;
    /* If newfd held a file or pipe endpoint, drop its ref first so
       pipe refcounts stay honest. Console fds persist. */
    if (fds[newfd].type == FD_FILE ||
        fds[newfd].type == FD_PIPE_READ ||
        fds[newfd].type == FD_PIPE_WRITE) {
        fd_drop_ref(&fds[newfd]);
    }
    fds[newfd] = fds[oldfd];
    /* The new fd is a fresh reference to the same underlying pipe
       — bump the corresponding reader/writer count so the ref
       accounting matches. */
    if (fds[newfd].type == FD_PIPE_READ)  pipe_add_reader((pipe_t *)fds[newfd].pipe);
    if (fds[newfd].type == FD_PIPE_WRITE) pipe_add_writer((pipe_t *)fds[newfd].pipe);
    return newfd;
}

ssize_t fd_read(int fd, void *buf, size_t n) {
    fd_entry_t *fds = current_fds();
    if (!fds || fd < 0 || fd >= FD_MAX) return -1;
    fd_entry_t *e = &fds[fd];
    if (e->type == FD_CONSOLE) {
        char *out = buf;
        size_t i = 0;
        while (i < n) {
            char c = serial_getchar();
            out[i++] = c;
            if (c == '\n') break;
        }
        return (ssize_t)i;
    }
    if (e->type == FD_FILE) {
        ssize_t r = vfs_read(e->path, buf, n, (off_t)e->offset);
        if (r > 0) e->offset += (uint64_t)r;
        return r;
    }
    if (e->type == FD_PIPE_READ)
        return pipe_read((pipe_t *)e->pipe, buf, n);
    return -1;
}

ssize_t fd_write(int fd, const void *buf, size_t n) {
    fd_entry_t *fds = current_fds();
    if (!fds || fd < 0 || fd >= FD_MAX) return -1;
    fd_entry_t *e = &fds[fd];
    if (e->type == FD_CONSOLE) {
        const char *s = buf;
        for (size_t i = 0; i < n; i++) {
            if (s[i] == '\n') serial_putchar('\r');
            serial_putchar(s[i]);
        }
        return (ssize_t)n;
    }
    if (e->type == FD_FILE) {
        ssize_t r = vfs_write(e->path, buf, n, (off_t)e->offset);
        if (r > 0) e->offset += (uint64_t)r;
        return r;
    }
    if (e->type == FD_PIPE_WRITE)
        return pipe_write((pipe_t *)e->pipe, buf, n);
    return -1;
}

off_t fd_lseek(int fd, off_t off, int whence) {
    fd_entry_t *fds = current_fds();
    if (!fds || fd < 0 || fd >= FD_MAX) return (off_t)-1;
    fd_entry_t *e = &fds[fd];
    if (e->type != FD_FILE) return (off_t)-1;
    uint64_t new_off = 0;
    struct vfs_stat st;
    switch (whence) {
    case 0: new_off = (uint64_t)off; break;
    case 1: new_off = e->offset + (uint64_t)off; break;
    case 2:
        if (vfs_stat(e->path, &st) != 0) return (off_t)-1;
        new_off = (uint64_t)st.size + (uint64_t)off; break;
    default: return (off_t)-1;
    }
    e->offset = new_off;
    return (off_t)new_off;
}

/* --- Signals ------------------------------------------------- */

int64_t process_signal(int signo, uint64_t handler) {
    if (signo <= 0 || signo >= NSIG) return -1;
    if (signo == SIG_KILL || signo == SIG_STOP) return -1;
    process_t *p = process_current();
    if (!p) return -1;
    uint64_t prev = p->sig_handlers[signo];
    p->sig_handlers[signo] = handler;
    return (int64_t)prev;
}

int process_kill(uint32_t pid, int signo) {
    process_t *target = process_get(pid);
    if (!target) return -1;
    if (signo <= 0 || signo >= NSIG) return -1;
    if (signo == SIG_KILL) {
        target->alive = false;
        if (target->task) target->task->state = TASK_DEAD;
        return 0;
    }
    target->sig_pending |= (1u << signo);
    return 0;
}

uint32_t process_set_alarm(uint32_t secs) {
    process_t *p = process_current();
    if (!p) return 0;
    uint32_t prev = p->alarm_ticks;
    p->alarm_ticks = secs * 100;
    return (prev + 99) / 100;
}

void process_tick_alarms(void) {
    for (int i = 0; i < PROCESS_MAX; i++) {
        process_t *p = &processes[i];
        if (!p->alive) continue;
        if (p->alarm_ticks > 0) {
            p->alarm_ticks--;
            if (p->alarm_ticks == 0) p->sig_pending |= (1u << SIG_ALRM);
        }
    }
}

void process_sigreturn(registers_t *regs) {
    process_t *p = process_current();
    if (!p || !p->sig_delivering) return;
    *regs = p->sig_saved_regs;
    p->sig_delivering = false;
}

/* Stack helpers — write a value onto a user stack reachable via
   the current PML4 (HHDM-backed). */
static int user_push_u64(uint64_t *rsp, uint64_t val) {
    uint64_t *pml4 = process_current() ? process_current()->task->pml4 : 0;
    if (!pml4) return -1;
    *rsp -= 8;
    uint64_t page = *rsp & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t phys = vmm_get_physical_in(pml4, page);
    if (!phys) return -1;
    *(uint64_t *)((uint8_t *)phys_to_virt_low(phys) + (*rsp - page)) = val;
    return 0;
}

/* The SIGRETURN trampoline lives in each process's user ABI —
   when the user-installed handler `ret`s, it pops the return
   address we pushed onto the stack and jumps there. The address
   must be a user-space function that invokes SYS_SIGRETURN. We
   store it per-process in sig_trampoline, set by the user via
   SYS_SIGNAL when installing the first handler (overwrites are
   idempotent since every ulib passes the same thunk). */
void process_deliver_pending_signals(registers_t *regs) {
    if (!regs) return;
    if ((regs->cs & 3) != 3) return;          /* not returning to ring 3 */
    process_t *p = process_current();
    if (!p) return;
    if (p->sig_delivering) return;
    if (p->sig_pending == 0) return;

    /* Pick the lowest-numbered pending signal. */
    int signo = 0;
    for (int i = 1; i < NSIG; i++) {
        if (p->sig_pending & (1u << i)) { signo = i; break; }
    }
    if (signo == 0) return;
    p->sig_pending &= ~(1u << signo);

    uint64_t h = p->sig_handlers[signo];
    if (h == 0) {
        /* SIG_DFL — terminate on catchable signals we model. */
        if (signo == SIG_INT || signo == SIG_ALRM || signo == SIG_TERM) {
            p->exit_code = 128 + signo;
            p->alive = false;
            if (p->task) p->task->state = TASK_DEAD;
        }
        return;
    }
    if (h == 1) return;                       /* SIG_IGN */

    /* Save the interrupted frame + mark in-delivery. */
    p->sig_saved_regs = *regs;
    p->sig_delivering = true;

    /* Push a fake return address — 0 — onto the user stack. The
       ulib sig thunk never returns here; it calls SYS_SIGRETURN.
       But `call h` semantics require SOMETHING at [rsp] to keep
       the stack shape main expects. */
    uint64_t user_rsp = regs->rsp;
    if (user_push_u64(&user_rsp, 0) < 0) { p->sig_delivering = false; return; }

    regs->rsp = user_rsp;
    regs->rip = h;
    regs->rdi = (uint64_t)signo;
    /* Maintain 16-byte stack alignment at call boundary: we pushed
       one 8-byte word, so rsp%16 == 8. Compiler treats handler as
       a function after a call, so alignment is correct. */
}

void process_list_all(void) {
    kprintf("PID  PPID  NAME\n");
    for (int i = 0; i < PROCESS_MAX; i++) {
        process_t *p = &processes[i];
        if (!p->alive) continue;
        kprintf("%u  %u  %s\n", p->pid, p->parent_pid, p->name);
    }
}
