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

#define USER_STACK_TOP 0x0000000000800000ULL
#define USER_STACK_PAGES 4

static process_t processes[PROCESS_MAX];
static uint32_t next_pid = 1;

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
            return p;
        }
    }
    return 0;
}

/* --- User stack layout (SysV AMD64) --------------------------- */
/* The ABI requires 16-byte-aligned RSP *before* the first user
   instruction. We push argv pointers + strings + NULL envp + NULL
   auxv from the top down and land with RSP % 16 == 0. */
static uint64_t build_user_stack(uint64_t *pml4, process_t *p,
                                 int argc, const char **argv_src) {
    uint64_t stack_top = USER_STACK_TOP;
    uint64_t sp = stack_top;

    /* Copy argv strings to user stack. */
    uint64_t str_ptrs[SPAWN_ARGV_MAX];
    for (int i = argc - 1; i >= 0; i--) {
        uint64_t n = strlen(argv_src[i]) + 1;
        sp -= n;
        /* Write via identity-mapped phys of target frame. */
        for (uint64_t off = 0; off < n; off++) {
            uint64_t va = sp + off;
            uint64_t page = va & ~(uint64_t)(PAGE_SIZE - 1);
            uint64_t phys = vmm_get_physical_in(pml4, page);
            if (!phys) return 0;
            ((uint8_t *)phys_to_virt_low(phys))[va - page] = argv_src[i][off];
        }
        str_ptrs[i] = sp;
    }
    /* Align to 16 bytes. */
    sp &= ~(uint64_t)0xF;
    /* Auxv terminator (2 × u64 = 0). */
    sp -= 16;
    /* Envp terminator. */
    sp -= 8;
    /* Argv: NULL + pointers (argc+1 slots), placed so that
       (sp % 16) == 0 after the CPU does nothing (entry gets
       argc on stack per SysV: main(argc, argv, envp)).
       For SysV AMD64 the crt0 reads argc from [rsp], argv from
       [rsp+8..], envp after NULL. */
    int total_ptrs = argc + 1 /*argv NULL*/ + 1 /*envp NULL already reserved*/;
    (void)total_ptrs;
    sp -= 8;                            /* argv NULL */
    for (int i = argc - 1; i >= 0; i--) {
        sp -= 8;
    }
    sp -= 8;                            /* argc */

    /* Align to 16 now, padding upward if needed. */
    if (sp & 0xF) sp &= ~(uint64_t)0xF;

    /* Now write the stack contents. */
    uint64_t wp = sp;
    /* argc */
    uint64_t *slot;
    #define WRITE_U64(addr, val)                                             \
        do {                                                                  \
            uint64_t _a = (addr);                                             \
            uint64_t _pg = _a & ~(uint64_t)(PAGE_SIZE - 1);                   \
            uint64_t _ph = vmm_get_physical_in(pml4, _pg);                    \
            if (!_ph) return 0;                                               \
            *(uint64_t *)((uint8_t *)phys_to_virt_low(_ph) + (_a - _pg)) =    \
                (val);                                                        \
        } while (0)
    (void)slot;
    WRITE_U64(wp, (uint64_t)argc); wp += 8;
    for (int i = 0; i < argc; i++) { WRITE_U64(wp, str_ptrs[i]); wp += 8; }
    WRITE_U64(wp, 0); wp += 8;          /* argv NULL */
    WRITE_U64(wp, 0); wp += 8;          /* envp NULL */
    WRITE_U64(wp, 0); wp += 8;          /* auxv AT_NULL key */
    WRITE_U64(wp, 0); wp += 8;          /* auxv AT_NULL val */

    (void)p;
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

    uint64_t rsp = build_user_stack(pml4, p, argc, argv_local);
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

    for (int i = 0; i < FD_MAX; i++) child->fds[i] = parent->fds[i];

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
                               char *const argv[]) {
    process_t *p = process_current();
    if (!p) return -1;

    /* Validate the new image before destroying the old one. */
    if (elf_validate(elf, size) != 0) return -1;

    /* Snapshot argv before we blow away user pages. Copy strings
       into the process's spawn buffer. */
    const char *argv_local[SPAWN_ARGV_MAX];
    int argc = 0;
    char *buf = p->spawn_argv_buf;
    uint64_t buf_used = 0;
    if (argv) {
        while (argv[argc] && argc < SPAWN_ARGV_MAX - 1) {
            uint64_t len = 0;
            while (argv[argc][len]) len++;
            len++;
            if (buf_used + len > SPAWN_ARGV_BUF) return -1;
            memcpy(buf + buf_used, argv[argc], (uint32_t)len);
            argv_local[argc] = buf + buf_used;
            buf_used += len;
            argc++;
        }
    } else {
        argv_local[0] = name ? name : "";
        argc = 1;
    }
    argv_local[argc] = 0;

    /* Drop old user image in the current PML4. Kernel half (PML4
       [256..511]) is untouched so we keep running. */
    uint64_t *pml4 = p->task->pml4;
    vmm_unmap_user_space(pml4);

    /* Load new ELF into the same PML4. */
    uint64_t entry = elf_load(elf, size, pml4);
    if (!entry) return -1;

    /* New user stack. */
    for (int i = 0; i < USER_STACK_PAGES; i++) {
        uint64_t va = USER_STACK_TOP - (i + 1) * PAGE_SIZE;
        uint64_t frame = pmm_alloc_frame();
        if (!frame) return -1;
        memset(phys_to_virt_low(frame), 0, PAGE_SIZE);
        vmm_map_in(pml4, va, frame, VMM_FLAG_WRITE | VMM_FLAG_USER);
    }

    uint64_t rsp = build_user_stack(pml4, p, argc, argv_local);
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

int fd_close(int fd) {
    fd_entry_t *fds = current_fds();
    if (!fds || fd < 0 || fd >= FD_MAX) return -1;
    if (fds[fd].type == FD_NONE) return -1;
    /* Console fds 0..2 stay open across close(). Files drop to NONE. */
    if (fds[fd].type == FD_CONSOLE) return 0;
    fds[fd].type = FD_NONE;
    fds[fd].path[0] = 0;
    fds[fd].offset = 0;
    fds[fd].flags  = 0;
    return 0;
}

ssize_t fd_read(int fd, void *buf, size_t n) {
    fd_entry_t *fds = current_fds();
    if (!fds || fd < 0 || fd >= FD_MAX) return -1;
    fd_entry_t *e = &fds[fd];
    if (e->type == FD_CONSOLE) {
        /* Blocking single-line read from the serial input ring. */
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

void process_list_all(void) {
    kprintf("PID  PPID  NAME\n");
    for (int i = 0; i < PROCESS_MAX; i++) {
        process_t *p = &processes[i];
        if (!p->alive) continue;
        kprintf("%u  %u  %s\n", p->pid, p->parent_pid, p->name);
    }
}
