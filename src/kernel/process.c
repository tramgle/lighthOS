#include "kernel/process.h"
#include "kernel/elf.h"
#include "kernel/debug.h"
#include "kernel/pipe.h"
#include "drivers/console.h"
#include "fs/vfs.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "mm/heap.h"
#include "lib/string.h"
#include "lib/kprintf.h"

/* Maintain pipe refcounts when duplicating or tearing down an fd entry.
   `inherit_pipe_ref` bumps on copy (dup2, fork, spawn's fd-table copy).
   `drop_pipe_ref` is the counterpart for fd_close / overwrites. */
static void inherit_pipe_ref(const fd_entry_t *f) {
    if (!f || !f->pipe) return;
    if (f->type == FD_PIPE_READ) pipe_add_reader((pipe_t *)f->pipe);
    else if (f->type == FD_PIPE_WRITE) pipe_add_writer((pipe_t *)f->pipe);
}

static void drop_pipe_ref(fd_entry_t *f) {
    if (!f || !f->pipe) return;
    if (f->type == FD_PIPE_READ) pipe_close_reader((pipe_t *)f->pipe);
    else if (f->type == FD_PIPE_WRITE) pipe_close_writer((pipe_t *)f->pipe);
    f->pipe = NULL;
}

extern void jump_to_usermode(uint32_t entry, uint32_t user_stack);

#define USER_STACK_BASE 0x0BFF0000
#define USER_STACK_SIZE (64 * 1024)  /* 64KB */
#define USER_STACK_TOP  (USER_STACK_BASE + USER_STACK_SIZE)

static process_t processes[PROCESS_MAX];
static uint32_t next_pid = 0;
static uint32_t foreground_pgid = 0;

void process_set_foreground(uint32_t pgid) { foreground_pgid = pgid; }
uint32_t process_get_foreground(void) { return foreground_pgid; }

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

/* Strip `root` prefix from `buf` in place. Used to convert a
   host-absolute path produced by process_resolve_path back into a
   chroot-local path suitable for storage in cwd. No-op if `root` is
   "/" or empty. Leaves buf unchanged if it doesn't start with root. */
void process_strip_root_prefix(char *buf, const char *root) {
    if (!buf || !root || !*root) return;
    if (root[0] == '/' && root[1] == '\0') return;
    int rlen = 0; while (root[rlen]) rlen++;
    int blen = 0; while (buf[blen]) blen++;
    if (blen < rlen) return;
    for (int i = 0; i < rlen; i++) if (buf[i] != root[i]) return;
    /* Next char after the prefix must be '/' or end-of-string —
       otherwise "/disk" would spuriously strip from "/disknovel". */
    if (buf[rlen] != '/' && buf[rlen] != '\0') return;
    int j = 0;
    for (int i = rlen; i <= blen; i++) buf[j++] = buf[i];
    if (buf[0] == '\0') { buf[0] = '/'; buf[1] = '\0'; }
}

int process_resolve_path(const char *path, char *out, int out_size) {
    if (!path || !out || out_size < 2) return -1;

    process_t *p = process_current();
    const char *cwd = (p && p->cwd[0]) ? p->cwd : "/";

    /* Build a joined absolute string (relative to the process's
       chroot view) in a scratch buffer, then canonicalize into a
       second scratch. canon_into's existing clamp prevents `..` from
       escaping "/" — which in chrooted processes IS the chroot
       boundary, since cwd/root are expressed in the chroot's frame. */
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

    char canonical[VFS_MAX_PATH];
    if (canon_into(joined, canonical, sizeof canonical) != 0) return -1;

    /* No chroot (or trivial chroot "/"): canonical IS the host path. */
    const char *rt = (p && p->root[0]) ? p->root : "/";
    int chrooted = (rt[0] == '/' && rt[1] != '\0');
    if (!chrooted) {
        int i = 0;
        while (canonical[i] && i < out_size - 1) { out[i] = canonical[i]; i++; }
        out[i] = '\0';
        return 0;
    }

    /* Chrooted: out = root + canonical. Special case: canonical == "/"
       means "the chroot root itself" — output is just `rt`. */
    int oi = 0;
    while (rt[oi] && oi < out_size - 1) { out[oi] = rt[oi]; oi++; }
    if (!(canonical[0] == '/' && canonical[1] == '\0')) {
        int ci = 0;
        while (canonical[ci] && oi < out_size - 1) out[oi++] = canonical[ci++];
    }
    out[oi] = '\0';
    return 0;
}

/* Default-action terminate. Caller holds whatever locking the context
   requires (IRQ-disabled for signal_foreground, plain scheduling for
   process_signal). */
static void sig_default_terminate(process_t *p, int signo) {
    p->exit_code = 128 + signo;
    p->alive = false;
    p->task->state = TASK_DEAD;
    process_t *parent = process_get(p->parent_pid);
    if (parent && parent->task && parent->task->state == TASK_BLOCKED) {
        parent->task->state = TASK_READY;
    }
}

static void sig_default_stop(process_t *p) {
    p->task->state = TASK_STOPPED;
    process_t *parent = process_get(p->parent_pid);
    if (parent && parent->task && parent->task->state == TASK_BLOCKED) {
        parent->task->state = TASK_READY;
    }
}

/* Queue `signo` for delivery when `p` next returns to user mode.
   Unblocks a blocked task so it gets a chance to run the handler. */
static void sig_queue(process_t *p, int signo) {
    p->sig_pending |= (1u << signo);
    if (p->task && p->task->state == TASK_BLOCKED) {
        p->task->state = TASK_READY;
    }
}

/* Deliver a signal to every process in the foreground group. IRQ-safe.
   Used by Ctrl-C (SIG_INT) and Ctrl-Z (SIG_STOP). If the target
   has a user handler installed (and the signal is catchable), queue
   via the pending bitmap for delivery on next return-to-user. */
static void signal_foreground(int signo) {
    uint32_t eflags;
    __asm__ volatile ("pushf; pop %0" : "=r"(eflags));
    __asm__ volatile ("cli");

    if (foreground_pgid == 0) goto out;

    for (int i = 0; i < PROCESS_MAX; i++) {
        process_t *p = &processes[i];
        if (!p->alive || !p->task) continue;
        if (p->pgid != foreground_pgid) continue;
        if (p->pid == 0) continue;  /* never kill init */
        if (p->task->state == TASK_DEAD) continue;

        if (signo == SIG_INT || signo == SIG_KILL) {
            /* SIG_KILL is uncatchable; SIG_INT respects user handlers. */
            uint32_t h = (signo < (int)NSIG) ? p->sig_handlers[signo] : 0;
            if (signo == SIG_INT && h > 1) {
                sig_queue(p, signo);
                dlog("[signal] SIGINT queued -> pid %u (%s)\n", p->pid, p->name);
            } else if (signo == SIG_INT && h == 1) {
                /* SIG_IGN: drop. */
                dlog("[signal] SIGINT ignored -> pid %u (%s)\n", p->pid, p->name);
            } else {
                dlog("[signal] SIG%d term -> pid %u (%s)\n", signo, p->pid, p->name);
                sig_default_terminate(p, signo);
            }
        } else if (signo == SIG_STOP) {
            /* SIG_STOP is uncatchable by spec — apply default. */
            dlog("[signal] SIGSTOP -> pid %u (%s)\n", p->pid, p->name);
            sig_default_stop(p);
        }
    }

out:
    if (eflags & 0x200) __asm__ volatile ("sti");
}

void process_kill_foreground(void) { signal_foreground(SIG_INT); }
void process_stop_foreground(void) { signal_foreground(SIG_STOP); }

/* Non-IRQ signal dispatch. `target > 0`: match pid. `target < 0`:
   match pgid (= -target). Returns 0 if at least one process was
   signalled, -1 otherwise. Catchable signals (INT/HUP) honor any
   user handler the target has installed; KILL/STOP/CONT are always
   handled with their default action. */
int process_signal(int target, int signo) {
    int matched = 0;
    int want_pgid = (target < 0);
    uint32_t key = (uint32_t)(want_pgid ? -target : target);

    for (int i = 0; i < PROCESS_MAX; i++) {
        process_t *p = &processes[i];
        if (!p->alive || !p->task) continue;
        if (want_pgid) { if (p->pgid != key) continue; }
        else           { if (p->pid  != key) continue; }
        if (p->pid == 0) continue;
        matched = 1;

        if (signo == SIG_KILL) {
            sig_default_terminate(p, signo);
        } else if (signo == SIG_INT || signo == SIG_HUP) {
            uint32_t h = (signo < (int)NSIG) ? p->sig_handlers[signo] : 0;
            if (h == 1) {
                /* SIG_IGN */
            } else if (h > 1) {
                sig_queue(p, signo);
            } else {
                sig_default_terminate(p, signo);
            }
        } else if (signo == SIG_STOP) {
            if (p->task->state != TASK_DEAD) sig_default_stop(p);
        } else if (signo == SIG_CONT) {
            if (p->task->state == TASK_STOPPED) p->task->state = TASK_READY;
        }
    }
    return matched ? 0 : -1;
}

int32_t process_sig_install(int signo, uint32_t handler) {
    if (signo < 1 || signo >= (int)NSIG) return -1;
    if (signo == SIG_KILL || signo == SIG_STOP) return -1;
    process_t *p = process_current();
    if (!p) return -1;
    uint32_t prev = p->sig_handlers[signo];
    p->sig_handlers[signo] = handler;
    return (int32_t)prev;
}

/* IRQ-safe: called from the timer interrupt at 100Hz. For each live
   process with an active alarm, tick it down one; on zero, queue
   SIG_ALRM via sig_pending. Delivery happens when that process next
   returns to user mode. Kept tight — PROCESS_MAX=16 so walking every
   slot every tick is cheap. */
void process_tick_alarms(void) {
    for (int i = 0; i < PROCESS_MAX; i++) {
        process_t *p = &processes[i];
        if (!p->alive || !p->task) continue;
        if (p->alarm_ticks == 0) continue;
        if (--p->alarm_ticks == 0) {
            sig_queue(p, SIG_ALRM);
        }
    }
}

uint32_t process_set_alarm(uint32_t secs) {
    process_t *p = process_current();
    if (!p) return 0;
    /* Remaining seconds (round up so "1 tick left" reports "1 sec"). */
    uint32_t prev_secs = (p->alarm_ticks + 99) / 100;
    p->alarm_ticks = secs * 100;
    return prev_secs;
}

void process_sigreturn(registers_t *regs) {
    process_t *p = process_current();
    if (!p || !p->sig_delivering) return;
    /* Restore the full user-mode frame that was saved at delivery.
       Because regs *is* the in-flight iret frame, overwriting it here
       means isr_common's popa/iret will land back at the interrupted
       user instruction with the original registers intact. */
    *regs = p->sig_saved_regs;
    p->sig_delivering = false;
}

void process_deliver_pending_signals(registers_t *regs) {
    if (!regs) return;
    /* Only rewrite frames that will iret back to ring 3. Ring-0 frames
       (kernel-thread yields, IRQs that preempted kernel code) must be
       left alone — we'd trash our own return path. */
    if ((regs->cs & 3) != 3) return;
    process_t *p = process_current();
    if (!p || !p->alive) return;
    if (p->sig_delivering) return;
    if (!p->sig_pending) return;

    /* Lowest-numbered pending signal wins. SIG_KILL/SIG_STOP can't be
       in the pending bitmap (sig_queue never adds them) — they go
       through the default path synchronously. */
    int signo = -1;
    for (int i = 1; i < (int)NSIG; i++) {
        if (p->sig_pending & (1u << i)) { signo = i; break; }
    }
    if (signo < 0) return;

    uint32_t h = p->sig_handlers[signo];
    p->sig_pending &= ~(1u << signo);
    if (h == 0) {
        /* SIG_DFL: terminate for catchable terminating signals
           (INT, HUP, ALRM, TERM). SIG_STOP/SIG_CONT never reach the
           pending bitmap — they're handled synchronously in
           signal_foreground / process_signal. */
        if (signo == SIG_INT  || signo == SIG_HUP ||
            signo == SIG_ALRM || signo == SIG_TERM) {
            sig_default_terminate(p, signo);
        }
        return;
    }
    if (h == 1) return;  /* SIG_IGN — drop */

    /* Snapshot the interrupted user frame verbatim. SYS_SIGRETURN
       copies it back to restore. */
    p->sig_saved_regs = *regs;

    /* Push argv for the handler onto the user stack: [signo] [retaddr].
       retaddr is set to 0 because the user-side thunk ends its flow with
       sys_sigreturn (never returns through this stack frame), but we
       still need to leave 4 bytes so the C calling convention's
       "argument at 4(%esp)" indexing resolves to signo. Writes go
       through the user's current CR3 directly — syscalls and IRQs from
       user mode leave user mappings installed. */
    uint32_t sp = regs->useresp;
    if (sp < 8 || sp > 0xC0000000u) return;  /* sanity — refuse wild esp */
    sp -= 4;
    *(uint32_t *)sp = (uint32_t)signo;
    sp -= 4;
    *(uint32_t *)sp = 0;  /* dummy return address; handler never returns here */

    regs->useresp = sp;
    regs->eip = h;
    /* Clear direction flag per sysv calling convention. IF stays set. */
    regs->eflags &= ~0x400u;
    p->sig_delivering = true;
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
    p->pgid = 0;
    strncpy(p->name, "kernel", PROCESS_NAME_MAX - 1);
    p->task = task_current();
    p->alive = true;
    p->brk = 0;
    strcpy(p->cwd, "/");
    strcpy(p->root, "/");

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
    /* Default: each new process is its own group leader. Shell can
       override via SYS_SETPGID to group pipeline segments under one
       pgid so Ctrl-C/Ctrl-Z hits them all. */
    p->pgid = p->pid;
    strncpy(p->name, name, PROCESS_NAME_MAX - 1);
    p->alive = true;
    p->brk = 0;

    /* Inherit cwd + fd table from the parent (or fall back to console
       stdio for pid 0's first child). Inheriting fds lets shell
       redirection and pipes propagate to children through sys_spawn
       the same way they would through fork+execve. Pipe fds need a
       reader/writer refcount bump for each copy. */
    process_t *parent = process_current();
    if (parent) {
        strcpy(p->cwd, parent->cwd);
        strcpy(p->root, parent->root[0] ? parent->root : "/");
        for (int i = 0; i < FD_MAX; i++) {
            p->fds[i] = parent->fds[i];
            inherit_pipe_ref(&p->fds[i]);
        }
    } else {
        strcpy(p->cwd, "/");
        strcpy(p->root, "/");
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

    /* Close all open FDs. Pipe ends must drop their ref so writers a
       downstream reader is still waiting on eventually hit zero. */
    for (int i = 0; i < FD_MAX; i++) {
        if (p->fds[i].type != FD_NONE) {
            drop_pipe_ref(&p->fds[i]);
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

    /* Transfer terminal focus to the child's pgid so Ctrl-C/Ctrl-Z
       reach everything in the pipeline. Restore the caller's fg pgid
       on return. */
    uint32_t prev_fg = foreground_pgid;
    foreground_pgid = child->pgid;

    /* Block until the child exits. A SIGSTOP on the fg pgid also wakes
       us up (the stop path unblocks our task), so we need to
       distinguish "stopped" from "dead" — if it's merely stopped,
       return control to the caller and drop the fg handoff so the
       shell can decide what to do next. */
    while (child->alive) {
        if (child->task && child->task->state == TASK_STOPPED) break;
        task_current()->state = TASK_BLOCKED;
        task_yield();
    }

    foreground_pgid = prev_fg;

    if (child->alive && child->task && child->task->state == TASK_STOPPED) {
        /* Report stopped: status high bits = signal. Don't reap. */
        if (status) *status = 0x7f;  /* Linux-ish WIFSTOPPED marker */
        return (int)child->pid;
    }

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
            out->pgid = p->pgid;
            out->state = p->task ? (uint32_t)p->task->state : 4;
            strncpy(out->name, p->name, PROCESS_NAME_MAX - 1);
            out->name[PROCESS_NAME_MAX - 1] = '\0';
            strncpy(out->root, p->root, VFS_MAX_PATH - 1);
            out->root[VFS_MAX_PATH - 1] = '\0';
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

/* ---- Spawn ELF Process ----
 *
 * Per-spawn context (entry point, stack top, argv snapshot) lives on
 * the target process_t. Keeping it per-process is what allows back-to-
 * back spawns without waiting — each child reads its own metadata from
 * its own struct in the trampoline. Shell pipelines rely on this. */

/* SysV auxv type tags. Matching Linux values so future ports of
   real-world userland code work unchanged. */
#define AT_NULL   0
#define AT_PHDR   3
#define AT_PHENT  4
#define AT_PHNUM  5
#define AT_PAGESZ 6
#define AT_BASE   7
#define AT_ENTRY  9

/* Allocate user stack pages, then lay down the SysV info block:
     [esp+0]   argc
     [esp+4]   argv[0]
     ...
     [esp+4+4*argc] NULL (argv terminator)
                    NULL (envp terminator — we have no env yet)
                    auxv pairs (if p->interp_entry != 0)
                    AT_NULL, 0
   `stack_top` must be mapped in the currently active PD. Returns
   the initial user ESP or 0 on OOM. */
static uint32_t setup_user_stack(uint32_t stack_top, process_t *p) {
    for (uint32_t addr = USER_STACK_BASE; addr < USER_STACK_TOP; addr += PAGE_SIZE) {
        uint32_t frame = pmm_alloc_frame();
        if (!frame) return 0;
        vmm_map_page(addr, frame, VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_USER);
    }

    int argc = p->spawn_argc;
    if (argc > SPAWN_ARGV_MAX) argc = SPAWN_ARGV_MAX;
    int has_auxv = (p->interp_entry != 0);

    /* Phase 1: copy argv strings to the top of the stack, capture
       their user-visible addresses for the argv pointer array. */
    uint32_t argv_str_ptrs[SPAWN_ARGV_MAX + 1];
    uint32_t sp = stack_top;
    for (int i = argc - 1; i >= 0; i--) {
        const char *s = p->spawn_argv_buf + p->spawn_argv_off[i];
        uint32_t len = strlen(s) + 1;
        sp -= len;
        memcpy((void *)sp, s, len);
        argv_str_ptrs[i] = sp;
    }
    argv_str_ptrs[argc] = 0;
    sp &= ~3u;

    /* Phase 2: info block, written bottom-up from a single computed
       base. Size: argc(4) + argv+null((argc+1)*4) + envp null(4) +
       auxv(2*4*N). N = 7 (PHDR/PHENT/PHNUM/BASE/ENTRY/PAGESZ/NULL)
       when dynamic, else 1 (AT_NULL alone). */
    uint32_t auxv_entries = has_auxv ? 7 : 1;
    uint32_t info_bytes = 4
                        + (uint32_t)(argc + 1) * 4
                        + 4
                        + auxv_entries * 8;
    sp -= info_bytes;
    sp &= ~3u;

    uint32_t *info = (uint32_t *)sp;
    uint32_t idx = 0;
    info[idx++] = (uint32_t)argc;
    for (int i = 0; i < argc; i++) info[idx++] = argv_str_ptrs[i];
    info[idx++] = 0;  /* argv terminator */
    info[idx++] = 0;  /* envp terminator (no environment yet) */
    if (has_auxv) {
        info[idx++] = AT_PHDR;   info[idx++] = p->main_phdr_vaddr;
        info[idx++] = AT_PHENT;  info[idx++] = sizeof(elf32_phdr_t);
        info[idx++] = AT_PHNUM;  info[idx++] = p->main_phnum;
        info[idx++] = AT_BASE;   info[idx++] = p->interp_base;
        info[idx++] = AT_ENTRY;  info[idx++] = p->main_entry;
        info[idx++] = AT_PAGESZ; info[idx++] = 4096;
    }
    info[idx++] = AT_NULL;
    info[idx++] = 0;

    return sp;
}

static void spawn_trampoline(void) {
    /* This runs in ring 0 as the new task. Pull our pending spawn
       metadata from our own process_t (not from shared globals — that
       raced when two spawns happened back-to-back, e.g. pipelines). */
    process_t *p = process_current();
    if (!p) { process_exit(-1); return; }

    uint32_t stk = p->spawn_stack_top;

    uint32_t sp = setup_user_stack(stk, p);
    if (!sp) { process_exit(-1); return; }

    if (p->brk == 0) {
        p->brk = 0x08100000;  /* default heap start after typical ELF load */
    }

    /* If the ELF declared PT_INTERP, the kernel loaded the interpreter
       at interp_base and we jump to its entry. The interpreter reads
       the SysV auxv from the stack, loads DT_NEEDED libraries, applies
       relocations, and eventually jumps to main's entry itself. For
       static binaries (interp_entry == 0) spawn_entry_point is the
       main's entry directly. */
    uint32_t entry = p->interp_entry ? p->interp_entry : p->spawn_entry_point;
    jump_to_usermode(entry, sp);
    /* Should never return */
    process_exit(-1);
}

/* Load the dynamic linker named in PT_INTERP alongside the main ELF.
   Our ld-vibeos.so.1 is a plain ET_EXEC statically linked to run at
   0x40000000, so we pass load_base=0 (p_vaddr already holds the
   runtime address). This sidesteps the self-relocation bootstrap
   problem that a PIC interpreter would need. Shared libraries are
   still ET_DYN — they're loaded later at runtime by the interpreter
   via SYS_MMAP_ANON, not by the kernel.
   Returns the interp entry point (already the runtime address at
   0x40000000+), 0 on failure. AT_BASE in the auxv is always
   INTERP_BASE so the interp knows where it lives. */
#define INTERP_BASE 0x40000000u
static uint32_t load_interp(const char *path, uint32_t *pd) {
    struct vfs_stat st;
    if (vfs_stat(path, &st) != 0) {
        kprintf("interp: %s not found\n", path);
        return 0;
    }
    void *buf = kmalloc(st.size);
    if (!buf) {
        kprintf("interp: out of memory reading %s\n", path);
        return 0;
    }
    ssize_t n = vfs_read(path, buf, st.size, 0);
    if (n != (ssize_t)st.size) {
        kfree(buf);
        kprintf("interp: short read on %s\n", path);
        return 0;
    }
    if (elf_validate(buf, st.size) != 0) {
        kfree(buf);
        kprintf("interp: %s not a valid ELF\n", path);
        return 0;
    }
    uint32_t entry = elf_load(buf, st.size, pd);
    kfree(buf);
    if (!entry) {
        kprintf("interp: failed to map %s segments\n", path);
        return 0;
    }
    return entry;
}

static void snapshot_argv_into(process_t *p, const char *path, char *const argv[]) {
    p->spawn_argc = 0;
    uint32_t used = 0;

    /* If caller didn't supply an argv, synthesize one containing just
       the program path so main always sees argc >= 1. */
    char *const fallback_argv[] = { (char *)path, NULL };
    char *const *src = (argv && argv[0]) ? argv : fallback_argv;

    for (int i = 0; src[i] && p->spawn_argc < SPAWN_ARGV_MAX; i++) {
        uint32_t len = strlen(src[i]) + 1;
        if (used + len > SPAWN_ARGV_BUF) break;  /* truncate */
        p->spawn_argv_off[p->spawn_argc] = used;
        memcpy(p->spawn_argv_buf + used, src[i], len);
        used += len;
        p->spawn_argc++;
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
    if (!entry) {
        kfree(buf);
        vmm_free_pd(child_pd);
        kprintf("spawn: failed to load %s\n", path);
        return -1;
    }

    /* If the main ELF names an interpreter (PT_INTERP), also load that
       into the child PD at INTERP_BASE. The interpreter's entry becomes
       the first user EIP; main's entry is handed off via AT_ENTRY in
       the SysV auxv. Static binaries (no PT_INTERP) leave these fields
       zero and setup_user_stack omits the auxv block entirely. */
    char interp_path[VFS_MAX_PATH];
    int has_interp = elf_find_interp(buf, st.size,
                                     interp_path, sizeof interp_path);
    uint32_t interp_entry = 0;
    uint32_t main_phdrs = has_interp ? elf_phdr_vaddr(buf, st.size) : 0;
    uint32_t main_phnum = ((const elf32_ehdr_t *)buf)->e_phnum;
    if (has_interp) {
        interp_entry = load_interp(interp_path, child_pd);
        if (!interp_entry) {
            kfree(buf);
            vmm_free_pd(child_pd);
            return -1;
        }
    }
    kfree(buf);

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
    /* Store entry point + argv snapshot on the child's process_t so the
       trampoline can pick them up when it runs. Keeping this per-process
       (rather than in shared statics) is what lets back-to-back spawns
       — shell pipelines — work without clobbering each other. */
    proc->spawn_entry_point = entry;
    proc->spawn_stack_top = USER_STACK_TOP;
    proc->main_entry      = entry;
    proc->main_phdr_vaddr = main_phdrs;
    proc->main_phnum      = main_phnum;
    proc->interp_base     = has_interp ? INTERP_BASE : 0;
    proc->interp_entry    = interp_entry;
    snapshot_argv_into(proc, path, argv);

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
       so copying is the simplest thing that matches single-threaded use.
       process_alloc already copied the fds and bumped pipe refs; we
       overwrite that here with the parent's current table and bump
       again only for the new additions. Simpler: clear what
       process_alloc set, then copy fresh. */
    for (int i = 0; i < FD_MAX; i++) {
        drop_pipe_ref(&child->fds[i]);
        child->fds[i] = parent->fds[i];
        inherit_pipe_ref(&child->fds[i]);
    }
    strncpy(child->cwd, parent->cwd, VFS_MAX_PATH - 1);
    strncpy(child->root, parent->root[0] ? parent->root : "/", VFS_MAX_PATH - 1);
    /* Inherit installed signal handlers across fork (POSIX). Do NOT
       copy the pending bitmap or delivering flag — those are per-task
       transient state, not part of the dispositions. */
    for (int i = 0; i < (int)NSIG; i++) child->sig_handlers[i] = parent->sig_handlers[i];
    child->sig_pending = 0;
    child->sig_delivering = false;

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
       caller's address space. Store into *our own* process_t — execve
       replaces the current image so argv lives on `p` itself. */
    char kpath[VFS_MAX_PATH];
    strncpy(kpath, path, VFS_MAX_PATH - 1);
    kpath[VFS_MAX_PATH - 1] = '\0';
    snapshot_argv_into(p, kpath, argv);

    /* Signal handlers were registered against the OLD image's address
       space — the new image won't have the same functions. Reset all
       dispositions to SIG_DFL (POSIX also keeps SIG_IGN across exec;
       we simplify and drop everything). */
    for (int i = 0; i < (int)NSIG; i++) p->sig_handlers[i] = 0;
    p->sig_pending = 0;
    p->sig_delivering = false;

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
    /* Reset any stale interp bookkeeping from the previous image. */
    p->main_entry = p->main_phdr_vaddr = p->main_phnum = 0;
    p->interp_base = p->interp_entry = 0;

    uint32_t entry = elf_load(buf, st.size, pd);
    if (!entry) {
        kfree(buf);
        kprintf("execve: load failed, killing process\n");
        process_exit(-1);
        return -1;  /* unreachable */
    }

    /* PT_INTERP handling — mirror process_spawn. */
    char interp_path[VFS_MAX_PATH];
    int has_interp = elf_find_interp(buf, st.size,
                                     interp_path, sizeof interp_path);
    if (has_interp) {
        p->main_entry      = entry;
        p->main_phdr_vaddr = elf_phdr_vaddr(buf, st.size);
        p->main_phnum      = ((const elf32_ehdr_t *)buf)->e_phnum;
        p->interp_base     = INTERP_BASE;
        p->interp_entry    = load_interp(interp_path, pd);
        if (!p->interp_entry) {
            kfree(buf);
            kprintf("execve: failed to load interp %s\n", interp_path);
            process_exit(-1);
            return -1;
        }
    }
    kfree(buf);

    uint32_t sp = setup_user_stack(USER_STACK_TOP, p);
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
       the new ELF's entry point in ring 3. For dynamic binaries, the
       first EIP is the interpreter; it reads the SysV auxv we just
       pushed and eventually jumps to the main exec's entry.
       0x1B = user CS, 0x23 = user DS/SS. */
    regs->eip = p->interp_entry ? p->interp_entry : entry;
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
    drop_pipe_ref(&p->fds[fd]);
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
       (type + path + offset + flags + pipe ptr) so the two fds share
       identical state. Non-pipe file descriptors don't share a
       file-description — subsequent writes from either fd keep their
       own offset. Pipe fds share the pipe_t via the pipe pointer;
       bump its reader/writer refcount to reflect the new handle. */
    drop_pipe_ref(&p->fds[newfd]);
    p->fds[newfd] = p->fds[oldfd];
    inherit_pipe_ref(&p->fds[newfd]);
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
    case FD_PIPE_READ:
        return pipe_read((pipe_t *)f->pipe, buf, count);
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
    case FD_PIPE_WRITE:
        return pipe_write((pipe_t *)f->pipe, buf, count);
    default:
        return -1;
    }
}

/* Allocate two fd slots, create a fresh pipe, install FD_PIPE_READ in
   fds[0] and FD_PIPE_WRITE in fds[1]. Returns 0 on success and fills
   out_fds with the two fd numbers; -1 on OOM or fd exhaustion. */
int fd_pipe(int out_fds[2]) {
    process_t *p = process_current();
    if (!p || !out_fds) return -1;

    int rfd = -1, wfd = -1;
    for (int i = 3; i < FD_MAX; i++) {
        if (p->fds[i].type != FD_NONE) continue;
        if (rfd < 0) rfd = i;
        else if (wfd < 0) { wfd = i; break; }
    }
    if (rfd < 0 || wfd < 0) return -1;

    pipe_t *pp = pipe_create();
    if (!pp) return -1;

    memset(&p->fds[rfd], 0, sizeof(fd_entry_t));
    p->fds[rfd].type = FD_PIPE_READ;
    p->fds[rfd].pipe = pp;
    pipe_add_reader(pp);

    memset(&p->fds[wfd], 0, sizeof(fd_entry_t));
    p->fds[wfd].type = FD_PIPE_WRITE;
    p->fds[wfd].pipe = pp;
    pipe_add_writer(pp);

    out_fds[0] = rfd;
    out_fds[1] = wfd;
    return 0;
}
