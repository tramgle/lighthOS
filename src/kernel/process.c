#include "kernel/process.h"
#include "kernel/elf.h"
#include "kernel/debug.h"
#include "drivers/console.h"
#include "fs/vfs.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "mm/heap.h"
#include "lib/string.h"
#include "lib/kprintf.h"

extern void jump_to_usermode(uint32_t entry, uint32_t user_stack);

#define USER_STACK_BASE 0x0BFF0000
#define USER_STACK_SIZE (64 * 1024)  /* 64KB */
#define USER_STACK_TOP  (USER_STACK_BASE + USER_STACK_SIZE)

static process_t processes[PROCESS_MAX];
static uint32_t next_pid = 0;
static uint32_t foreground_pid = 0;

void process_set_foreground(uint32_t pid) { foreground_pid = pid; }
uint32_t process_get_foreground(void) { return foreground_pid; }

/* Walk `src` (already absolute), copying canonical segments into `out`.
   Collapses `.` and `..`. Output always starts with '/' and never has
   a trailing '/', except for root which is "/" alone. */
static int canon_into(const char *src, char *out, int out_size) {
    if (out_size < 2) return -1;
    int oi = 0;
    out[oi++] = '/';
    int i = (src[0] == '/') ? 1 : 0;
    while (src[i]) {
        int start = i;
        while (src[i] && src[i] != '/') i++;
        int seglen = i - start;
        if (seglen == 0) {
            /* consecutive '/' */
        } else if (seglen == 1 && src[start] == '.') {
            /* '.' — stay */
        } else if (seglen == 2 && src[start] == '.' && src[start + 1] == '.') {
            /* '..' — pop the last segment */
            if (oi > 1) {
                oi--;  /* drop trailing '/' */
                while (oi > 0 && out[oi - 1] != '/') oi--;
            }
        } else {
            if (oi + seglen + 1 >= out_size) return -1;
            for (int j = 0; j < seglen; j++) out[oi++] = src[start + j];
            out[oi++] = '/';
        }
        if (src[i] == '/') i++;
    }
    if (oi > 1 && out[oi - 1] == '/') oi--;
    out[oi] = '\0';
    return 0;
}

int process_resolve_path(const char *path, char *out, int out_size) {
    if (!path || !out || out_size < 2) return -1;

    process_t *p = process_current();
    const char *cwd = (p && p->cwd[0]) ? p->cwd : "/";

    /* Build a joined absolute string in a scratch buffer, then
       canonicalize into `out`. */
    char joined[VFS_MAX_PATH * 2];
    int ji = 0;
    if (path[0] == '/') {
        while (path[ji] && ji < (int)sizeof(joined) - 1) {
            joined[ji] = path[ji]; ji++;
        }
    } else {
        int ci = 0;
        while (cwd[ci] && ji < (int)sizeof(joined) - 1) {
            joined[ji++] = cwd[ci++];
        }
        if (ji == 0 || joined[ji - 1] != '/') {
            if (ji < (int)sizeof(joined) - 1) joined[ji++] = '/';
        }
        int pi = 0;
        while (path[pi] && ji < (int)sizeof(joined) - 1) {
            joined[ji++] = path[pi++];
        }
    }
    joined[ji] = '\0';

    return canon_into(joined, out, out_size);
}

void process_kill_foreground(void) {
    /* Called from serial IRQ context. Disable interrupts around the
       multi-step state mutations so an inline exit path or another
       interrupt can't catch us half-updated. Single-CPU system —
       cli/sti is sufficient, no spinlock needed. */
    uint32_t eflags;
    __asm__ volatile ("pushf; pop %0" : "=r"(eflags));
    __asm__ volatile ("cli");

    if (foreground_pid == 0) goto out;
    process_t *p = process_get(foreground_pid);
    if (!p || !p->task) goto out;
    /* Never kill the kernel/shell-as-pid-0; refuse to take the system
       down via Ctrl-C. */
    if (p->pid == 0) goto out;
    /* If the target is already on its way out, leave it alone — the
       exit path will finish its own cleanup. */
    if (!p->alive || p->task->state == TASK_DEAD) goto out;

    dlog("[signal] Ctrl-C -> pid %u (%s)\n", p->pid, p->name);
    p->exit_code = 130;   /* 128 + SIGINT */
    p->alive = false;
    p->task->state = TASK_DEAD;
    /* Unblock the parent if it was waiting on us — waitpid's loop
       checks `alive` after each yield, so once the parent runs again
       it'll fall through and return. */
    process_t *parent = process_get(p->parent_pid);
    if (parent && parent->task && parent->task->state == TASK_BLOCKED) {
        parent->task->state = TASK_READY;
    }

out:
    if (eflags & 0x200) __asm__ volatile ("sti");
}

/* Map from task ID to process — simple linear scan */
static process_t *find_by_task(task_t *t) {
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (processes[i].alive && processes[i].task == t)
            return &processes[i];
    }
    return NULL;
}

process_t *process_current(void) {
    return find_by_task(task_current());
}

process_t *process_get(uint32_t pid) {
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (processes[i].pid == pid && processes[i].alive)
            return &processes[i];
    }
    return NULL;
}

void process_init(void) {
    memset(processes, 0, sizeof(processes));

    /* Create process 0 for the currently running kernel context (idle task / test runner) */
    process_t *p = &processes[0];
    p->pid = next_pid++;
    p->parent_pid = 0;
    strncpy(p->name, "kernel", PROCESS_NAME_MAX - 1);
    p->task = task_current();
    p->alive = true;
    p->brk = 0;
    strcpy(p->cwd, "/");

    /* Set up stdin/stdout/stderr as console */
    p->fds[0].type = FD_CONSOLE;  /* stdin */
    p->fds[1].type = FD_CONSOLE;  /* stdout */
    p->fds[2].type = FD_CONSOLE;  /* stderr */

    serial_printf("[process] Initialized, pid 0 = kernel\n");
}

process_t *process_alloc(const char *name) {
    /* Find a free slot */
    process_t *p = NULL;
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (!processes[i].alive && processes[i].name[0] == '\0') {
            p = &processes[i];
            break;
        }
    }
    if (!p) {
        /* Reuse a dead slot */
        for (int i = 1; i < PROCESS_MAX; i++) {
            if (!processes[i].alive) {
                p = &processes[i];
                break;
            }
        }
    }
    if (!p) {
        dlog("process_alloc: no free slots\n");
        return NULL;
    }

    memset(p, 0, sizeof(process_t));
    p->pid = next_pid++;
    p->parent_pid = process_current() ? process_current()->pid : 0;
    strncpy(p->name, name, PROCESS_NAME_MAX - 1);
    p->alive = true;
    p->brk = 0;

    /* Inherit cwd + fd table from the parent (or fall back to console
       stdio for pid 0's first child). Inheriting fds lets shell
       redirection and future pipes propagate to children through
       sys_spawn the same way they would through fork+execve. */
    process_t *parent = process_current();
    if (parent) {
        strcpy(p->cwd, parent->cwd);
        for (int i = 0; i < FD_MAX; i++) p->fds[i] = parent->fds[i];
    } else {
        strcpy(p->cwd, "/");
        p->fds[0].type = FD_CONSOLE;
        p->fds[1].type = FD_CONSOLE;
        p->fds[2].type = FD_CONSOLE;
    }

    return p;
}

process_t *process_create(const char *name, void (*entry)(void)) {
    process_t *p = process_alloc(name);
    if (!p) return NULL;

    p->task = task_create(name, entry);
    if (!p->task) {
        memset(p, 0, sizeof(process_t));
        return NULL;
    }

    dlog("[process] Created pid %u: %s\n", p->pid, p->name);
    return p;
}

void process_exit(int code) {
    process_t *p = process_current();
    if (!p) return;

    p->exit_code = code;
    p->alive = false;

    /* Close all open FDs */
    for (int i = 0; i < FD_MAX; i++) {
        if (p->fds[i].type != FD_NONE) {
            p->fds[i].type = FD_NONE;
        }
    }

    /* Unblock any parent waiting on us */
    process_t *parent = process_get(p->parent_pid);
    if (parent && parent->task && parent->task->state == TASK_BLOCKED) {
        parent->task->state = TASK_READY;
    }

    dlog("[process] pid %u (%s) exited with code %d\n", p->pid, p->name, code);

    /* Kill the task */
    p->task->state = TASK_DEAD;
    task_yield();
    for (;;) __asm__ volatile ("hlt");
}

int process_waitpid(uint32_t pid, int *status) {
    process_t *child = NULL;

    /* Find the child process (alive or recently dead) */
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (processes[i].pid == pid) {
            child = &processes[i];
            break;
        }
    }
    if (!child) return -1;

    /* Transfer terminal focus to the child so Ctrl-C goes to it. Keep
       the old value so we can restore the caller's foreground status
       when the child exits. */
    uint32_t prev_fg = foreground_pid;
    foreground_pid = child->pid;

    /* If child is still alive, block until it exits */
    while (child->alive) {
        task_current()->state = TASK_BLOCKED;
        task_yield();
    }

    foreground_pid = prev_fg;

    if (status) *status = child->exit_code;

    /* Clean up the child's name so the slot can be reused */
    uint32_t ret_pid = child->pid;
    child->name[0] = '\0';

    return (int)ret_pid;
}

int process_info(uint32_t idx, proc_info_t *out) {
    if (!out) return -1;
    uint32_t seen = 0;
    for (int i = 0; i < PROCESS_MAX; i++) {
        process_t *p = &processes[i];
        if (!p->alive) continue;
        if (seen == idx) {
            out->pid = p->pid;
            out->parent_pid = p->parent_pid;
            out->state = p->task ? (uint32_t)p->task->state : 3;
            strncpy(out->name, p->name, PROCESS_NAME_MAX - 1);
            out->name[PROCESS_NAME_MAX - 1] = '\0';
            return 0;
        }
        seen++;
    }
    return -1;
}

void process_list_all(void) {
    kprintf("PID\tPPID\tSTATE\tNAME\n");
    kprintf("---\t----\t-----\t----\n");
    for (int i = 0; i < PROCESS_MAX; i++) {
        process_t *p = &processes[i];
        if (!p->alive) continue;
        const char *st = "???";
        if (p->task) {
            switch (p->task->state) {
            case TASK_READY:   st = "READY"; break;
            case TASK_RUNNING: st = "RUN"; break;
            case TASK_BLOCKED: st = "BLOCK"; break;
            case TASK_DEAD:    st = "DEAD"; break;
            }
        }
        kprintf("%u\t%u\t%s\t%s\n", p->pid, p->parent_pid, st, p->name);
    }
}

/* ---- Spawn ELF Process ---- */

/* Per-spawn context passed to the task entry trampoline */
static uint32_t spawn_entry_point;
static uint32_t spawn_stack_top;

/* Kernel snapshot of the spawn's argv. The parent's memory holding the
   original strings may be overwritten by the time the child's trampoline
   runs, so we copy everything into kernel space first and materialize it
   onto the child's user stack in spawn_trampoline. */
#define SPAWN_ARGV_MAX 16
#define SPAWN_ARGV_BUF 512
static int      spawn_argc;
static char     spawn_argv_buf[SPAWN_ARGV_BUF];
static uint32_t spawn_argv_off[SPAWN_ARGV_MAX];  /* byte offsets into spawn_argv_buf */
static uint32_t spawn_argv_used;                  /* bytes consumed in spawn_argv_buf */

/* Allocate user stack pages, then lay down argc/argv at the top of the
   stack. `stack_top` must be a valid virtual address in the currently
   active PD — we write through virtual addresses, relying on vmm_map_page
   to invlpg each new mapping. Returns the initial user ESP. */
static uint32_t setup_user_stack(uint32_t stack_top, int argc,
                                 const char *argv_buf,
                                 const uint32_t *argv_off) {
    for (uint32_t addr = USER_STACK_BASE; addr < USER_STACK_TOP; addr += PAGE_SIZE) {
        uint32_t frame = pmm_alloc_frame();
        if (!frame) return 0;
        vmm_map_page(addr, frame, VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_USER);
    }

    if (argc > SPAWN_ARGV_MAX) argc = SPAWN_ARGV_MAX;

    uint32_t user_argv_ptrs[SPAWN_ARGV_MAX + 1];
    uint32_t sp = stack_top;
    for (int i = argc - 1; i >= 0; i--) {
        const char *s = argv_buf + argv_off[i];
        uint32_t len = strlen(s) + 1;
        sp -= len;
        sp &= ~3u;
        memcpy((void *)sp, s, len);
        user_argv_ptrs[i] = sp;
    }
    user_argv_ptrs[argc] = 0;

    sp -= (uint32_t)(argc + 1) * sizeof(uint32_t);
    sp &= ~3u;
    memcpy((void *)sp, user_argv_ptrs, (argc + 1) * sizeof(uint32_t));
    uint32_t user_argv_addr = sp;

    sp -= 4;
    *(uint32_t *)sp = user_argv_addr;
    sp -= 4;
    *(uint32_t *)sp = (uint32_t)argc;

    return sp;
}

static void spawn_trampoline(void) {
    /* This runs in ring 0 as the new task.
       Set up user stack pages, then jump to ring 3 at the ELF entry point. */
    uint32_t entry = spawn_entry_point;
    uint32_t stk = spawn_stack_top;

    uint32_t sp = setup_user_stack(stk, spawn_argc, spawn_argv_buf, spawn_argv_off);
    if (!sp) { process_exit(-1); return; }

    /* Set process break just after the loaded ELF for sbrk */
    process_t *p = process_current();
    if (p && p->brk == 0) {
        p->brk = 0x08100000;  /* default heap start after typical ELF load */
    }

    jump_to_usermode(entry, sp);
    /* Should never return */
    process_exit(-1);
}

static void snapshot_argv(const char *path, char *const argv[]) {
    spawn_argc = 0;
    spawn_argv_used = 0;

    /* If caller didn't supply an argv, synthesize one containing just
       the program path so main always sees argc >= 1. */
    char *const fallback_argv[] = { (char *)path, NULL };
    char *const *src = (argv && argv[0]) ? argv : fallback_argv;

    for (int i = 0; src[i] && spawn_argc < SPAWN_ARGV_MAX; i++) {
        uint32_t len = strlen(src[i]) + 1;
        if (spawn_argv_used + len > SPAWN_ARGV_BUF) break;  /* truncate */
        spawn_argv_off[spawn_argc] = spawn_argv_used;
        memcpy(spawn_argv_buf + spawn_argv_used, src[i], len);
        spawn_argv_used += len;
        spawn_argc++;
    }
}

int process_spawn(const char *path, char *const argv[]) {
    /* Read the ELF from the VFS */
    struct vfs_stat st;
    if (vfs_stat(path, &st) != 0) {
        /* Silent — the shell uses vfs_stat-lookup failures to implement
           a PATH-style fallback (/bin -> /disk/bin). The caller prints
           its own "command not found" when all paths are exhausted. */
        return -1;
    }

    void *buf = kmalloc(st.size);
    if (!buf) {
        kprintf("spawn: out of memory for %u bytes\n", st.size);
        return -1;
    }

    ssize_t n = vfs_read(path, buf, st.size, 0);
    if (n != (ssize_t)st.size) {
        kfree(buf);
        kprintf("spawn: failed to read %s\n", path);
        return -1;
    }

    /* Validate and load into a fresh page directory. Loading into a
       dedicated PD — rather than the parent's — is the change that
       stops child ELFs from overwriting the parent's text at the
       shared 0x08048000 virtual address. */
    if (elf_validate(buf, st.size) != 0) {
        kfree(buf);
        kprintf("spawn: %s is not a valid ELF\n", path);
        return -1;
    }

    uint32_t *child_pd = vmm_new_pd();
    if (!child_pd) {
        kfree(buf);
        kprintf("spawn: out of memory for page directory\n");
        return -1;
    }

    uint32_t entry = elf_load(buf, st.size, child_pd);
    kfree(buf);

    if (!entry) {
        vmm_free_pd(child_pd);
        kprintf("spawn: failed to load %s\n", path);
        return -1;
    }

    /* Store entry point + argv snapshot for the trampoline (single-
       threaded spawn for now — mutating these globals while another
       spawn is mid-flight would be racy). */
    spawn_entry_point = entry;
    spawn_stack_top = USER_STACK_TOP;
    snapshot_argv(path, argv);

    /* Extract just the filename for the process name */
    const char *name = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/') name = p + 1;
    }

    process_t *proc = process_create(name, spawn_trampoline);
    if (!proc) {
        vmm_free_pd(child_pd);
        return -1;
    }
    /* Bind the freshly loaded address space to the new task. The next
       schedule() tick into this task will `mov cr3, child_pd`, making
       spawn_trampoline's user-stack mapping and argv copy run in the
       child's address space. */
    proc->task->pd = child_pd;

    return (int)proc->pid;
}

/* ---- Fork ---- */

/* Copy every present USER page in `src_pd` into freshly allocated frames
   mapped at the same virtual addresses in `dst_pd`. Kernel PDEs (0..3)
   are already shared at PD allocation time, so we start at 4. */
static int fork_copy_user_space(uint32_t *src_pd, uint32_t *dst_pd) {
    for (uint32_t pdi = 4; pdi < 1024; pdi++) {
        uint32_t pde = src_pd[pdi];
        if (!(pde & VMM_FLAG_PRESENT)) continue;

        uint32_t *src_pt = (uint32_t *)(pde & 0xFFFFF000);
        for (uint32_t pti = 0; pti < 1024; pti++) {
            uint32_t pte = src_pt[pti];
            if (!(pte & VMM_FLAG_PRESENT)) continue;
            if (!(pte & VMM_FLAG_USER)) continue;

            uint32_t va = (pdi << 22) | (pti << 12);
            uint32_t new_frame = pmm_alloc_frame();
            if (!new_frame) {
                kprintf("fork: out of memory copying page 0x%x\n", va);
                return -1;
            }
            uint32_t old_frame = pte & 0xFFFFF000;
            memcpy((void *)new_frame, (void *)old_frame, PAGE_SIZE);
            vmm_map_in(dst_pd, va, new_frame, pte & 0xFFF);
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
    child->brk = parent->brk;
    /* Duplicate FD table wholesale — file offsets are shared in the POSIX
       fork model, but we don't have shared-state file descriptions yet
       so copying is the simplest thing that matches single-threaded use. */
    for (int i = 0; i < FD_MAX; i++) child->fds[i] = parent->fds[i];
    strncpy(child->cwd, parent->cwd, VFS_MAX_PATH - 1);

    task_t *t = task_alloc(parent->name);
    if (!t) {
        child->alive = false;
        child->name[0] = '\0';
        return -1;
    }

    uint32_t *child_pd = vmm_new_pd();
    if (!child_pd) {
        /* task_alloc already claimed the slot; mark DEAD so task_create's
           reuse path reclaims it. Stack was kmalloc'd — the dead-slot
           reaper will kfree it. */
        t->state = TASK_DEAD;
        child->alive = false;
        child->name[0] = '\0';
        return -1;
    }

    if (fork_copy_user_space(parent->task->pd, child_pd) != 0) {
        vmm_free_pd(child_pd);
        t->state = TASK_DEAD;
        child->alive = false;
        child->name[0] = '\0';
        return -1;
    }

    t->pd = child_pd;

    /* Synthesize the child's kernel stack so its next schedule() pops
       out of isr_common as if it had just completed the same int 0x80.
       Mirror the parent's registers_t wholesale, then force eax=0 so
       the child sees fork() == 0. */
    uint32_t stack_top = t->stack_base + TASK_STACK_SIZE;
    uint32_t sp = stack_top - sizeof(registers_t);
    registers_t *child_regs = (registers_t *)sp;
    memcpy(child_regs, parent_regs, sizeof(registers_t));
    child_regs->eax = 0;
    t->esp = sp;

    child->task = t;
    task_insert_ready(t);

    dlog("[process] fork: pid %u -> %u\n", parent->pid, child->pid);
    return (int)child->pid;
}

/* ---- Execve ---- */

int process_execve(registers_t *regs, const char *path, char *const argv[]) {
    process_t *p = process_current();
    if (!p) return -1;

    /* Snapshot path + argv into kernel buffers before we rip up the
       caller's address space. */
    char kpath[VFS_MAX_PATH];
    strncpy(kpath, path, VFS_MAX_PATH - 1);
    kpath[VFS_MAX_PATH - 1] = '\0';
    snapshot_argv(kpath, argv);

    /* Read + validate the ELF while the old mappings are still live. */
    struct vfs_stat st;
    if (vfs_stat(kpath, &st) != 0) {
        kprintf("execve: %s not found\n", kpath);
        return -1;
    }

    void *buf = kmalloc(st.size);
    if (!buf) {
        kprintf("execve: out of memory for %u bytes\n", st.size);
        return -1;
    }

    ssize_t n = vfs_read(kpath, buf, st.size, 0);
    if (n != (ssize_t)st.size) {
        kfree(buf);
        kprintf("execve: failed to read %s\n", kpath);
        return -1;
    }

    if (elf_validate(buf, st.size) != 0) {
        kfree(buf);
        kprintf("execve: %s is not a valid ELF\n", kpath);
        return -1;
    }

    /* Point of no return: tear down old user mappings. After this the
       only code path is to load the new image or die — the task has
       no usable user address space until we finish setup. */
    uint32_t *pd = p->task->pd;
    vmm_unmap_user_space(pd);

    uint32_t entry = elf_load(buf, st.size, pd);
    kfree(buf);
    if (!entry) {
        kprintf("execve: load failed, killing process\n");
        process_exit(-1);
        return -1;  /* unreachable */
    }

    uint32_t sp = setup_user_stack(USER_STACK_TOP, spawn_argc,
                                   spawn_argv_buf, spawn_argv_off);
    if (!sp) {
        kprintf("execve: user stack setup failed\n");
        process_exit(-1);
        return -1;
    }

    p->brk = 0x08100000;

    /* Update process name from the last path component. */
    const char *name = kpath;
    for (const char *c = kpath; *c; c++) {
        if (*c == '/') name = c + 1;
    }
    strncpy(p->name, name, PROCESS_NAME_MAX - 1);
    p->name[PROCESS_NAME_MAX - 1] = '\0';
    strncpy(p->task->name, name, sizeof(p->task->name) - 1);

    /* Rewrite the in-flight iret frame so the syscall return lands at
       the new ELF's entry point in ring 3. 0x1B = user CS, 0x23 = user DS/SS. */
    regs->eip = entry;
    regs->useresp = sp;
    regs->cs = 0x1B;
    regs->ss = 0x23;
    regs->ds = regs->es = regs->fs = regs->gs = 0x23;
    regs->eflags = (regs->eflags & ~0x200u) | 0x200u;  /* IF set */
    regs->eax = 0;

    dlog("[process] execve: pid %u now running %s\n", p->pid, p->name);
    return 0;
}

/* ---- File Descriptor Operations ---- */

int fd_open(const char *path, uint32_t flags) {
    process_t *p = process_current();
    if (!p) return -1;

    /* Find free FD (start at 3, skip stdin/stdout/stderr) */
    int fd = -1;
    for (int i = 3; i < FD_MAX; i++) {
        if (p->fds[i].type == FD_NONE) {
            fd = i;
            break;
        }
    }
    if (fd < 0) return -1;

    /* Paths reach fd_open already canonicalized by the SYS_OPEN
       handler (process_resolve_path). No further resolution needed. */
    struct vfs_stat st;
    if (vfs_stat(path, &st) != 0) {
        if (flags & O_CREAT) {
            if (vfs_create(path, VFS_FILE) != 0) return -1;
        } else {
            return -1;
        }
    }

    p->fds[fd].type = FD_FILE;
    strncpy(p->fds[fd].path, path, VFS_MAX_PATH - 1);
    p->fds[fd].path[VFS_MAX_PATH - 1] = '\0';
    p->fds[fd].offset = 0;
    p->fds[fd].flags = flags;

    /* O_APPEND: start writes at end-of-file. Stat may fail for a
       freshly-created file; offset 0 is correct in that case. */
    if (flags & O_APPEND) {
        if (vfs_stat(path, &st) == 0) p->fds[fd].offset = st.size;
    }

    return fd;
}

int fd_close(int fd) {
    process_t *p = process_current();
    if (!p || fd < 0 || fd >= FD_MAX) return -1;
    if (p->fds[fd].type == FD_NONE) return -1;
    p->fds[fd].type = FD_NONE;
    return 0;
}

int fd_dup2(int oldfd, int newfd) {
    process_t *p = process_current();
    if (!p) return -1;
    if (oldfd < 0 || oldfd >= FD_MAX || newfd < 0 || newfd >= FD_MAX) return -1;
    if (p->fds[oldfd].type == FD_NONE) return -1;
    if (oldfd == newfd) return newfd;
    /* Close newfd if it was in use, then copy the entire entry
       (type + path + offset + flags) so the two fds share identical
       state. They don't share a file-description — subsequent writes
       from either fd keep their own offset. Good enough for shell
       redirection's single-use case. */
    p->fds[newfd] = p->fds[oldfd];
    return newfd;
}

ssize_t fd_read(int fd, void *buf, size_t count) {
    process_t *p = process_current();
    if (!p || fd < 0 || fd >= FD_MAX) return -1;

    fd_entry_t *f = &p->fds[fd];
    switch (f->type) {
    case FD_CONSOLE:
        return console_read(buf, count);
    case FD_FILE: {
        ssize_t n = vfs_read(f->path, buf, count, f->offset);
        if (n > 0) f->offset += n;
        return n;
    }
    default:
        return -1;
    }
}

off_t fd_lseek(int fd, off_t offset, int whence) {
    process_t *p = process_current();
    if (!p || fd < 0 || fd >= FD_MAX) return -1;
    fd_entry_t *f = &p->fds[fd];
    if (f->type != FD_FILE) return -1;

    off_t new_off;
    if (whence == 0) new_off = offset;
    else if (whence == 1) new_off = (off_t)f->offset + offset;
    else if (whence == 2) {
        struct vfs_stat st;
        if (vfs_stat(f->path, &st) != 0) return -1;
        new_off = (off_t)st.size + offset;
    } else return -1;

    if (new_off < 0) return -1;
    f->offset = (uint32_t)new_off;
    return new_off;
}

ssize_t fd_write(int fd, const void *buf, size_t count) {
    process_t *p = process_current();
    if (!p || fd < 0 || fd >= FD_MAX) return -1;

    fd_entry_t *f = &p->fds[fd];
    switch (f->type) {
    case FD_CONSOLE:
        return console_write(buf, count);
    case FD_FILE: {
        ssize_t n = vfs_write(f->path, buf, count, f->offset);
        if (n > 0) f->offset += n;
        return n;
    }
    default:
        return -1;
    }
}
