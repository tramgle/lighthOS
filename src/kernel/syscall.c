/* x86_64 syscall dispatcher.
 *
 * INT 0x80 entry; RAX = syscall number; args in RDI/RSI/RDX/R10/R8/R9
 * per SysV AMD64 "syscall" convention. Return in RAX.
 */

#include "kernel/syscall.h"
#include "kernel/isr.h"
#include "kernel/process.h"
#include "kernel/task.h"
#include "kernel/timer.h"
#include "lib/kprintf.h"
#include "fs/vfs.h"
#include "fs/fstab.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "fs/blkdev.h"
#include "mm/heap.h"
#include "lib/string.h"
#include "drivers/serial.h"
#include "drivers/vga.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "drivers/console.h"

/* Thin wrappers so the SYS_EXECVE case body reads cleanly even
   though heap.h's names don't have module-prefix namespacing. */
static inline void *kmalloc_wrap(uint64_t n) { return kmalloc(n); }
static inline void  kfree_wrap(void *p)      { kfree(p); }

/* Resolve a user-supplied path through the current process's
   cwd + chroot root. Returns NULL on overflow. The returned
   pointer is to a static per-call buffer — caller must not hold
   across syscall dispatches. */
/* --- strace ring. Single target pid. Entry recorded on syscall
   entry (or on exit-process), with the syscall's return value
   stamped once the dispatcher fills it in.  trace_target_pid == 0
   means tracing is off. */
#define STRACE_RING 256
struct strace_entry {
    uint32_t seq;
    uint32_t pid;
    uint32_t num;
    uint32_t exited;         /* 1 for the exited-process marker */
    uint64_t a1, a2, a3, a4;
    int64_t  ret;
};
static struct strace_entry trace_ring[STRACE_RING];
static uint32_t trace_next_seq;
static uint32_t trace_target_pid;

/* SYS_REGIONS: pmm_region_iter takes a callback with no context
   pointer, so the dispatch stashes its state here for the duration
   of the call. Serial syscall dispatch makes this safe. */
uint64_t regions_target;
uint64_t regions_seen;
struct region_out regions_result;
int regions_found;

void regions_collect_cb(uint32_t start_frame, uint32_t len, bool used) {
    if (regions_found) return;
    if (regions_seen == regions_target) {
        regions_result.start_addr = (uint64_t)start_frame * PAGE_SIZE;
        regions_result.end_addr   = (uint64_t)(start_frame + len) * PAGE_SIZE;
        regions_result.used       = used ? 1 : 0;
        regions_result._pad       = 0;
        regions_found = 1;
    }
    regions_seen++;
}

static void trace_record(uint32_t pid, uint32_t num,
                         uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                         int64_t ret, int exited) {
    if (trace_target_pid == 0 || pid != trace_target_pid) return;
    struct strace_entry *e = &trace_ring[trace_next_seq % STRACE_RING];
    e->seq = trace_next_seq;
    e->pid = pid; e->num = num;
    e->a1 = a1; e->a2 = a2; e->a3 = a3; e->a4 = a4;
    e->ret = ret; e->exited = (uint32_t)exited;
    trace_next_seq++;
}

void syscall_trace_exited(uint32_t pid, int code) {
    trace_record(pid, 0, 0, 0, 0, 0, (int64_t)code, 1);
    /* Disable tracing once target exits so subsequent processes
       don't pollute the ring. */
    if (pid == trace_target_pid) trace_target_pid = 0;
}

static char resolve_buf[VFS_MAX_PATH];
static const char *resolve_path(const char *user_path) {
    if (!user_path) return 0;
    if (process_resolve_path(user_path, resolve_buf, sizeof(resolve_buf)) < 0)
        return 0;
    return resolve_buf;
}

#define SYS_EXIT     1
#define SYS_READ     3
#define SYS_WRITE    4
#define SYS_OPEN     5
#define SYS_CLOSE    6
#define SYS_WAITPID  7
#define SYS_UNLINK  10
#define SYS_STAT    18
#define SYS_LSEEK   19
#define SYS_GETPID  20
#define SYS_YIELD   24
#define SYS_MKDIR   39
#define SYS_MMAP_ANON 9
#define SYS_MOUNT    21
#define SYS_UMOUNT   22
#define SYS_ALARM    27
#define SYS_KILL     37
#define SYS_PIPE     42
#define SYS_SIGNAL   48
#define SYS_DUP2     63
#define SYS_FORK     57
#define SYS_SIGRETURN 119
#define SYS_SETPGID  109
#define SYS_GETPGID  108
#define SYS_CHROOT  161
#define SYS_MPROTECT 125
#define SYS_EXECVE  59
#define SYS_READDIR 89
#define SYS_SPAWN  120
#define SYS_SHUTDOWN 201
#define SYS_TIME       214
#define SYS_TRACEME    231
#define SYS_TRACE_READ 232

static void acpi_shutdown(void) {
    __asm__ volatile ("outw %0, %1" :: "a"((uint16_t)0x2000), "Nd"((uint16_t)0x604));
    for (;;) __asm__ volatile ("hlt");
}

/* struct the user sees for stat. Matches user/syscall.h's vfs_stat. */
struct user_vfs_stat {
    uint32_t inode;
    uint32_t type;
    uint32_t size;
};

registers_t *syscall_handler(registers_t *regs) {
    uint64_t num = regs->rax;
    uint64_t a1 = regs->rdi;
    uint64_t a2 = regs->rsi;
    uint64_t a3 = regs->rdx;
    uint64_t a4 = regs->r10;
    (void)a3; (void)a4;

    /* Entry-point trace hook. Snapshot args before dispatch
       overwrites regs->rax with the return value. */
    process_t *__cur = process_current();
    uint32_t __cur_pid = __cur ? __cur->pid : 0;
    int __traced = (__cur_pid == trace_target_pid);
    uint64_t __entry_pos = 0;
    if (__traced) {
        __entry_pos = trace_next_seq;
        trace_record(__cur_pid, (uint32_t)num, a1, a2, a3, a4, 0, 0);
    }

    switch (num) {
    case SYS_EXIT:
        if (__traced) syscall_trace_exited(__cur_pid, (int)a1);
        process_exit((int)a1);
        regs->rax = 0;
        break;

    case SYS_READ:
        regs->rax = (uint64_t)(int64_t)fd_read((int)a1,
            (void *)(uintptr_t)a2, (size_t)a3);
        break;

    case SYS_WRITE:
        regs->rax = (uint64_t)(int64_t)fd_write((int)a1,
            (const void *)(uintptr_t)a2, (size_t)a3);
        break;

    case SYS_OPEN: {
        const char *p = resolve_path((const char *)(uintptr_t)a1);
        regs->rax = p ? (uint64_t)(int64_t)fd_open(p, (uint32_t)a2)
                      : (uint64_t)(int64_t)-1;
        break;
    }

    case SYS_CLOSE:
        regs->rax = (uint64_t)(int64_t)fd_close((int)a1);
        break;

    case SYS_WAITPID: {
        int status = 0;
        int r = process_waitpid((uint32_t)a1, &status);
        if (a2) *(int *)(uintptr_t)a2 = status;
        regs->rax = (uint64_t)(int64_t)r;
        break;
    }

    case SYS_UNLINK: {
        const char *p = resolve_path((const char *)(uintptr_t)a1);
        regs->rax = p ? (uint64_t)(int64_t)vfs_unlink(p)
                      : (uint64_t)(int64_t)-1;
        break;
    }

    case SYS_STAT: {
        const char *p = resolve_path((const char *)(uintptr_t)a1);
        if (!p) { regs->rax = (uint64_t)(int64_t)-1; break; }
        struct vfs_stat kst;
        int r = vfs_stat(p, &kst);
        if (r == 0 && a2) {
            struct user_vfs_stat *u = (struct user_vfs_stat *)(uintptr_t)a2;
            u->inode = kst.inode;
            u->type  = kst.type;
            u->size  = kst.size;
        }
        regs->rax = (uint64_t)(int64_t)r;
        break;
    }

    case SYS_LSEEK:
        regs->rax = (uint64_t)(int64_t)fd_lseek((int)a1, (off_t)a2, (int)a3);
        break;

    case SYS_GETPID: {
        process_t *p = process_current();
        regs->rax = p ? p->pid : 0;
        break;
    }

    case SYS_YIELD:
        regs->rax = 0;
        break;

    case SYS_MKDIR: {
        const char *p = resolve_path((const char *)(uintptr_t)a1);
        regs->rax = p ? (uint64_t)(int64_t)vfs_mkdir(p)
                      : (uint64_t)(int64_t)-1;
        break;
    }

    case SYS_DUP2:
        regs->rax = (uint64_t)(int64_t)fd_dup2((int)a1, (int)a2);
        break;

    case SYS_MMAP_ANON: {
        /* mmap_anon(addr, len, prot) — allocate len-bytes rounded
           to PAGE_SIZE, map at addr with USER|flags. Returns addr
           on success, -1 on overlap or OOM. */
        uint64_t addr = a1 & ~(uint64_t)(PAGE_SIZE - 1);
        uint64_t len  = (a2 + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
        uint64_t prot = a3;
        if (len == 0 || addr == 0) { regs->rax = (uint64_t)(int64_t)-1; break; }
        uint64_t *pml4 = task_current_pml4();
        if (!pml4) { regs->rax = (uint64_t)(int64_t)-1; break; }
        /* reject overlap with any existing user mapping */
        for (uint64_t va = addr; va < addr + len; va += PAGE_SIZE) {
            if (vmm_get_physical_in(pml4, va)) {
                regs->rax = (uint64_t)(int64_t)-1; goto mmap_done;
            }
        }
        uint64_t flags = VMM_FLAG_USER;
        if (prot & 0x2) flags |= VMM_FLAG_WRITE;
        for (uint64_t va = addr; va < addr + len; va += PAGE_SIZE) {
            uint64_t frame = pmm_alloc_frame();
            if (!frame) { regs->rax = (uint64_t)(int64_t)-1; goto mmap_done; }
            memset(phys_to_virt_low(frame), 0, PAGE_SIZE);
            vmm_map_in(pml4, va, frame, flags);
        }
        regs->rax = addr;
mmap_done:
        break;
    }

    case SYS_MPROTECT: {
        uint64_t addr = a1 & ~(uint64_t)(PAGE_SIZE - 1);
        uint64_t len  = (a2 + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
        uint64_t prot = a3;
        uint64_t *pml4 = task_current_pml4();
        if (!pml4) { regs->rax = (uint64_t)(int64_t)-1; break; }
        uint64_t flags = VMM_FLAG_USER;
        if (prot & 0x2) flags |= VMM_FLAG_WRITE;
        int rc = 0;
        for (uint64_t va = addr; va < addr + len; va += PAGE_SIZE) {
            if (vmm_set_flags_in(pml4, va, flags) < 0) { rc = -1; break; }
        }
        regs->rax = (uint64_t)(int64_t)rc;
        break;
    }

    case SYS_MOUNT: {
        /* rdi=src, rsi=mountpoint, rdx=type, r10=flags */
        uint64_t a4 = regs->r10;
        const char *src   = (const char *)(uintptr_t)a1;
        const char *mp    = (const char *)(uintptr_t)a2;
        const char *type  = (const char *)(uintptr_t)a3;
        const char *flags = a4 ? (const char *)(uintptr_t)a4 : "rw";
        regs->rax = (uint64_t)(int64_t)fstab_do_mount(src, mp, type, flags);
        break;
    }

    case SYS_UMOUNT:
        regs->rax = (uint64_t)(int64_t)vfs_umount((const char *)(uintptr_t)a1);
        if (regs->rax == 0) {
            /* clear mount_path on the backing blkdev so lsblk shows
               detached state. best-effort — the blkdev lookup is
               by-mount-path which we don't store in vfs_mount, so
               skip the back-clear for now. */
        }
        break;

    case SYS_CHDIR: {
        /* Resolve the user path through the current cwd + chroot,
           then store as the new chroot-local cwd. Rejects anything
           that isn't a directory. */
        const char *path = (const char *)(uintptr_t)a1;
        process_t *p = process_current();
        if (!p) { regs->rax = (uint64_t)(int64_t)-1; break; }
        char resolved[VFS_MAX_PATH];
        if (process_resolve_path(path, resolved, sizeof resolved) < 0) {
            regs->rax = (uint64_t)(int64_t)-1; break;
        }
        struct vfs_stat st;
        if (vfs_stat(resolved, &st) != 0 || st.type != VFS_DIR) {
            regs->rax = (uint64_t)(int64_t)-1; break;
        }
        /* Strip the chroot root so what we store is chroot-local.
           resolved starts with p->root; skip that prefix. For an
           unrooted process (p->root == "/"), no prefix to strip —
           we keep the leading '/'. */
        const char *root = p->root[0] ? p->root : "/";
        int ri = 0;
        if (root[0] == '/' && root[1] == 0) {
            ri = 0;                 /* no chroot, keep full path */
        } else {
            while (root[ri] && resolved[ri] == root[ri]) ri++;
            if (root[ri] != 0) { regs->rax = (uint64_t)(int64_t)-1; break; }
        }
        const char *tail = resolved + ri;
        if (!*tail) tail = "/";
        int k = 0;
        while (tail[k] && k < VFS_MAX_PATH - 1) { p->cwd[k] = tail[k]; k++; }
        p->cwd[k] = 0;
        regs->rax = 0;
        break;
    }

    case SYS_GETCWD: {
        char *buf = (char *)(uintptr_t)a1;
        uint64_t cap = a2;
        process_t *p = process_current();
        if (!p || !buf || cap == 0) { regs->rax = (uint64_t)(int64_t)-1; break; }
        int n = 0;
        while (p->cwd[n] && (uint64_t)n < cap - 1) { buf[n] = p->cwd[n]; n++; }
        buf[n] = 0;
        regs->rax = (uint64_t)n;
        break;
    }

    case SYS_CHROOT: {
        process_t *p = process_current();
        if (!p) { regs->rax = (uint64_t)(int64_t)-1; break; }
        const char *target = resolve_path((const char *)(uintptr_t)a1);
        if (!target) { regs->rax = (uint64_t)(int64_t)-1; break; }
        struct vfs_stat st;
        if (vfs_stat(target, &st) != 0 || st.type != VFS_DIR) {
            regs->rax = (uint64_t)(int64_t)-1; break;
        }
        int n = 0;
        while (target[n] && n < VFS_MAX_PATH - 1) {
            p->root[n] = target[n]; n++;
        }
        while (n > 1 && p->root[n-1] == '/') n--;
        p->root[n] = 0;
        p->cwd[0] = '/'; p->cwd[1] = 0;
        regs->rax = 0;
        break;
    }

    case SYS_KILL:
        regs->rax = (uint64_t)(int64_t)process_kill((int32_t)a1, (int)a2);
        break;

    case SYS_SETPGID:
        regs->rax = (uint64_t)(int64_t)process_setpgid((uint32_t)a1, (uint32_t)a2);
        break;

    case SYS_GETPGID:
        regs->rax = process_getpgid((uint32_t)a1);
        break;

    case SYS_SIGNAL:
        regs->rax = (uint64_t)process_signal((int)a1, a2);
        break;

    case SYS_SIGRETURN:
        process_sigreturn(regs);
        /* regs has been rewritten to the saved user frame — no
           further dispatch. */
        return regs;

    case SYS_ALARM:
        regs->rax = (uint64_t)process_set_alarm((uint32_t)a1);
        break;

    case SYS_PIPE: {
        int pair[2];
        int r = fd_pipe(pair);
        if (r == 0 && a1) {
            int *out = (int *)(uintptr_t)a1;
            out[0] = pair[0]; out[1] = pair[1];
        }
        regs->rax = (uint64_t)(int64_t)r;
        break;
    }

    case SYS_FORK:
        regs->rax = (uint64_t)(int64_t)process_fork(regs);
        break;

    case SYS_EXECVE: {
        /* (path, argv, envp). Reads the ELF via vfs, unmaps the
           caller's user image, loads the new one, rewrites regs. */
        const char *upath = (const char *)(uintptr_t)a1;
        char *const *argv = (char *const *)(uintptr_t)a2;
        const char *path = resolve_path(upath);
        if (!path) { regs->rax = (uint64_t)(int64_t)-1; break; }
        /* Copy the resolved path into a stable kernel buffer — the
           resolve_buf is static per-call and execve's user-stack
           rebuild will issue more syscalls that stomp it. */
        char kpath[VFS_MAX_PATH];
        int ki = 0;
        while (path[ki] && ki < VFS_MAX_PATH - 1) { kpath[ki] = path[ki]; ki++; }
        kpath[ki] = 0;
        struct vfs_stat st;
        if (vfs_stat(kpath, &st) != 0 || st.size == 0) {
            regs->rax = (uint64_t)(int64_t)-1; break;
        }
        void *buf = kmalloc_wrap(st.size);
        if (!buf) { regs->rax = (uint64_t)(int64_t)-1; break; }
        ssize_t r = vfs_read(kpath, buf, st.size, 0);
        if (r != (ssize_t)st.size) {
            kfree_wrap(buf);
            regs->rax = (uint64_t)(int64_t)-1; break;
        }
        char *const *envp = (char *const *)(uintptr_t)a3;
        int rc = process_execve_from_memory(regs, kpath, buf, st.size, argv, envp);
        kfree_wrap(buf);
        if (rc < 0) regs->rax = (uint64_t)(int64_t)-1;
        /* Success: regs is already rewritten — when we return, the
           stub iretq's into the new image at entry with the fresh
           user stack we built. */
        break;
    }

    case SYS_READDIR: {
        const char *p = resolve_path((const char *)(uintptr_t)a1);
        if (!p) { regs->rax = (uint64_t)(int64_t)-1; break; }
        char name[VFS_MAX_NAME];
        uint32_t type = 0;
        int r = vfs_readdir(p, (uint32_t)a2, name, &type);
        if (r == 0 && a3) {
            /* User passes {char name[VFS_MAX_NAME]; uint32_t type;} */
            char *out = (char *)(uintptr_t)a3;
            for (int i = 0; i < VFS_MAX_NAME; i++) out[i] = name[i];
            *(uint32_t *)(out + VFS_MAX_NAME) = type;
        }
        regs->rax = (uint64_t)(int64_t)r;
        break;
    }

    case SYS_SPAWN: {
        /* Spawn a new process executing ELF at path `a1` with argv
           `a2` (NULL-terminated). Returns child pid or -1. Uses the
           same VFS-reading path as process_spawn_from_path, so it
           covers ramfs + FAT alike. Detaches — no parent linkage. */
        const char *p = resolve_path((const char *)(uintptr_t)a1);
        char *const *argv = (char *const *)(uintptr_t)a2;
        if (!p) { regs->rax = (uint64_t)(int64_t)-1; break; }
        regs->rax = (uint64_t)(int64_t)process_spawn_from_path(p, argv);
        break;
    }

    case SYS_TIME:
        regs->rax = timer_get_ticks();
        break;

    case SYS_PS: {
        struct proc_info *out = (struct proc_info *)(uintptr_t)a2;
        regs->rax = (uint64_t)(int64_t)process_info_at((uint32_t)a1, out);
        break;
    }

    case SYS_BLKDEVS: {
        struct blkdev_info {
            char     name[16];
            uint32_t total_sectors;
            char     mount_path[32];
            char     fs_type[16];
            uint32_t read_only;
        } *out = (struct blkdev_info *)(uintptr_t)a2;
        blkdev_t *dev = blkdev_nth((int)a1);
        if (!dev) { regs->rax = (uint64_t)(int64_t)-1; break; }
        for (int k = 0; k < 16; k++)  out->name[k]       = dev->name[k];
        out->total_sectors = dev->total_sectors;
        for (int k = 0; k < 32; k++) out->mount_path[k] = dev->mount_path[k];
        for (int k = 0; k < 16; k++) out->fs_type[k]    = dev->fs_type[k];
        out->read_only = dev->read_only;
        regs->rax = 0;
        break;
    }

    case SYS_TTY_RAW: {
        serial_set_raw((int)a1);
        regs->rax = 0;
        break;
    }

    case SYS_TCSETPGRP: {
        /* No ownership checks — we don't track a controlling tty per
           process. The shell uses this to hand the terminal to
           foreground pipelines and reclaim it on exit. */
        process_set_foreground((uint32_t)a1);
        regs->rax = 0;
        break;
    }

    case SYS_TTY_POLL:
        /* Non-blocking "is there a byte ready on the console?" test.
           Userspace uses this to back off probes (CSI-6n) that might
           never get answered AND for games like flappy to poll for
           input without blocking. Checks BOTH rings — otherwise
           PS/2 keystrokes on a VGA session go invisible. */
        regs->rax = (serial_has_data() || keyboard_has_key()) ? 1 : 0;
        break;

    case SYS_VGA_TEXT:
        /* Restore 80x25 text mode. The user-space framebuffer alias
           that SYS_VGA_GFX set up stays mapped — harmless, the
           process exits right after this in practice. */
        vga_text_enter();
        regs->rax = 0;
        break;

    case SYS_TTY_LASTSRC:
        regs->rax = (uint64_t)console_last_input_src();
        break;

    case SYS_MOUSE_POLL: {
        struct mouse_state_k {
            int32_t  x;
            int32_t  y;
            uint32_t buttons;
        } *out = (struct mouse_state_k *)(uintptr_t)a1;
        if (!out) { regs->rax = (uint64_t)(int64_t)-1; break; }
        int32_t x, y;
        uint32_t buttons;
        mouse_get_state(&x, &y, &buttons);
        out->x = x;
        out->y = y;
        out->buttons = buttons;
        regs->rax = 0;
        break;
    }

    case SYS_ISATTY: {
        /* Return 1 iff `a1` is an open fd on the console (keyboard +
           VGA / COM1 line discipline). Used by /bin/ls to decide
           whether to lay out in columns (tty) or one-per-line
           (pipe / file redirect). FD_FILE, FD_PIPE_*, FD_NONE all
           return 0. Bad fd also returns 0 — callers should treat
           "not a tty" as the conservative default. */
        int fd = (int)(int64_t)a1;
        process_t *p = process_current();
        if (!p || fd < 0 || fd >= FD_MAX) { regs->rax = 0; break; }
        regs->rax = (p->fds[fd].type == FD_CONSOLE) ? 1 : 0;
        break;
    }

    case SYS_PAUSE: {
        /* Block the task until any signal is delivered. The scheduler
           skips TASK_BLOCKED tasks entirely, so idle `hold` loops
           stop burning CPU. signal_group / process_kill flip the
           task back to TASK_READY when sig_pending is set. Return
           value is 0 (POSIX says -1/EINTR, but we don't have errno
           plumbing; zero is fine for our callers). */
        process_t *p = process_current();
        if (p && p->task) {
            p->task->state = TASK_BLOCKED;
            regs->rax = 0;
            return schedule(regs);
        }
        regs->rax = (uint64_t)(int64_t)-1;
        break;
    }

    case SYS_VGA_GFX: {
        /* Switch to VGA mode 13h and alias the framebuffer at a
           fixed user VA chosen by the caller (a1). No-ops if called
           twice — the caller gets back the same VA either way.
           Returns the user VA on success, -1 on failure. */
        uint64_t user_va = a1 & ~0xFFFULL;
        if (!user_va) { regs->rax = (uint64_t)(int64_t)-1; break; }
        uint64_t *pml4 = task_current_pml4();
        if (!pml4) { regs->rax = (uint64_t)(int64_t)-1; break; }
        for (int i = 0; i < VGA_MODE13_PAGES; i++) {
            vmm_map_in(pml4,
                       user_va + (uint64_t)i * PAGE_SIZE,
                       VGA_MODE13_PHYS + (uint64_t)i * PAGE_SIZE,
                       VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_USER);
        }
        vga_mode13_enter();
        /* Keep the mouse cursor inside the visible framebuffer so it
           doesn't wander off into unused clamp space while the user
           program redraws. Text mode's 80x25 doesn't need this —
           user programs that care about the cursor in text mode
           would manage their own coordinates. */
        mouse_set_extent(320, 200);
        regs->rax = user_va;
        break;
    }

    case SYS_TTY_WINSZ: {
        /* op=0: get — fill *a2=rows, *a3=cols (uint16_t pointers).
           op=1: set — a2=rows, a3=cols. Default 24x80 until something
           probes via CSI-6n and writes back. */
        uint64_t op = a1;
        if (op == 0) {
            uint16_t r, c;
            serial_get_winsize(&r, &c);
            if (a2) *(uint16_t *)(uintptr_t)a2 = r;
            if (a3) *(uint16_t *)(uintptr_t)a3 = c;
            regs->rax = 0;
        } else {
            serial_set_winsize((uint16_t)a2, (uint16_t)a3);
            regs->rax = 0;
        }
        break;
    }

    case SYS_TIMES: {
        /* Linux legacy times(2): writes {utime, stime, cutime, cstime}
           into *out and returns the current tick count. Ticks are 100
           Hz (1 tick = 10 ms). */
        struct tms_out {
            uint64_t utime;
            uint64_t stime;
            uint64_t cutime;
            uint64_t cstime;
        } *out = (struct tms_out *)(uintptr_t)a1;
        process_t *p = process_current();
        if (!p || !out) { regs->rax = (uint64_t)(int64_t)-1; break; }
        out->utime  = p->utime_ticks;
        out->stime  = p->stime_ticks;
        out->cutime = p->cutime_ticks;
        out->cstime = p->cstime_ticks;
        regs->rax = timer_get_ticks();
        break;
    }

    case SYS_MEMINFO: {
        struct meminfo {
            uint64_t total_kb;
            uint64_t free_kb;
        } *out = (struct meminfo *)(uintptr_t)a1;
        out->total_kb = (uint64_t)pmm_get_total_count() * 4u;   /* 4 KiB */
        out->free_kb  = (uint64_t)pmm_get_free_count()  * 4u;
        regs->rax = 0;
        break;
    }

    case SYS_PEEK: {
        /* Read `count` bytes of physical memory starting at `src` into
           the user's `dst` buffer. Source must lie within the first
           64 MiB (the HHDM window). Caps at 4 KiB per call to keep
           the kernel from being used as a giant memcpy engine. */
        uint64_t src   = a1;
        uint64_t dst   = a2;
        uint64_t count = a3;
        const uint64_t HHDM_LIMIT = 0x4000000ULL;   /* 64 MiB */
        const uint64_t PEEK_MAX   = 4096;
        if (count == 0) { regs->rax = 0; break; }
        if (count > PEEK_MAX || src + count < src ||
            src + count > HHDM_LIMIT) {
            regs->rax = (uint64_t)(int64_t)-1; break;
        }
        const uint8_t *k = (const uint8_t *)(KERNEL_HHDM_BASE + src);
        uint8_t *u = (uint8_t *)(uintptr_t)dst;
        for (uint64_t i = 0; i < count; i++) u[i] = k[i];
        regs->rax = count;
        break;
    }

    case SYS_PAGEMAP: {
        /* Walk the current PML4 for user VA `a1` and fill *a2. */
        struct pagemap_out *out = (struct pagemap_out *)(uintptr_t)a2;
        if (!out) { regs->rax = (uint64_t)(int64_t)-1; break; }
        uint64_t va = a1;
        uint64_t *pml4 = task_current_pml4();
        if (!pml4) { regs->rax = (uint64_t)(int64_t)-1; break; }

        uint32_t i_pml4 = (uint32_t)((va >> 39) & 0x1FF);
        uint32_t i_pdpt = (uint32_t)((va >> 30) & 0x1FF);
        uint32_t i_pd   = (uint32_t)((va >> 21) & 0x1FF);
        uint32_t i_pt   = (uint32_t)((va >> 12) & 0x1FF);

        out->pml4_idx = i_pml4; out->pdpt_idx = i_pdpt;
        out->pd_idx   = i_pd;   out->pt_idx   = i_pt;
        out->pml4e = out->pdpte = out->pde = out->pte = out->phys = 0;

        out->pml4e = pml4[i_pml4];
        if (!(out->pml4e & VMM_FLAG_PRESENT)) { regs->rax = 0; break; }
        uint64_t *pdpt = (uint64_t *)(uintptr_t)
            (KERNEL_HHDM_BASE + (out->pml4e & 0x000FFFFFFFFFF000ULL));
        out->pdpte = pdpt[i_pdpt];
        if (!(out->pdpte & VMM_FLAG_PRESENT)) { regs->rax = 0; break; }
        uint64_t *pd = (uint64_t *)(uintptr_t)
            (KERNEL_HHDM_BASE + (out->pdpte & 0x000FFFFFFFFFF000ULL));
        out->pde = pd[i_pd];
        if (!(out->pde & VMM_FLAG_PRESENT)) { regs->rax = 0; break; }
        uint64_t *pt = (uint64_t *)(uintptr_t)
            (KERNEL_HHDM_BASE + (out->pde & 0x000FFFFFFFFFF000ULL));
        out->pte = pt[i_pt];
        if (out->pte & VMM_FLAG_PRESENT) {
            out->phys = (out->pte & 0x000FFFFFFFFFF000ULL) | (va & 0xFFF);
        }
        regs->rax = 0;
        break;
    }

    case SYS_REGIONS: {
        /* Return the nth region reported by pmm_region_iter. */
        struct region_out *out = (struct region_out *)(uintptr_t)a2;
        if (!out) { regs->rax = (uint64_t)(int64_t)-1; break; }
        regions_target = a1;
        regions_seen   = 0;
        regions_found  = 0;
        pmm_region_iter(regions_collect_cb);
        if (regions_found) {
            *out = regions_result;
            regs->rax = 0;
        } else {
            regs->rax = (uint64_t)(int64_t)-1;
        }
        break;
    }

    case SYS_SHUTDOWN:
        kprintf("[kernel] shutdown requested by pid %u\n",
                process_current() ? process_current()->pid : 0);
        acpi_shutdown();
        break;

    case SYS_TRACEME:
        /* Set/clear the global trace target. pid=0 disables. */
        trace_target_pid = (uint32_t)a1;
        if (trace_target_pid) trace_next_seq = 0;
        regs->rax = 0;
        break;

    case SYS_TRACE_READ: {
        /* rdi=seq, rsi=*out.  Returns 0 on success, -1 past EOF. */
        uint32_t seq = (uint32_t)a1;
        if (seq >= trace_next_seq) { regs->rax = (uint64_t)(int64_t)-1; break; }
        if (trace_next_seq - seq > STRACE_RING) { regs->rax = (uint64_t)(int64_t)-1; break; }
        struct strace_entry *src = &trace_ring[seq % STRACE_RING];
        if (a2) *(struct strace_entry *)(uintptr_t)a2 = *src;
        regs->rax = 0;
        break;
    }

    default:
        kprintf("[syscall] unknown %lu from pid %u\n", num,
                process_current() ? process_current()->pid : 0);
        regs->rax = (uint64_t)(int64_t)-1;
        break;
    }

    /* Stamp the trace entry's return value now that the dispatcher
       has filled regs->rax. SYS_SIGRETURN returns early above — we
       don't record its ret. Same for SYS_EXIT since the process is
       already dead. */
    if (__traced && num != SYS_SIGRETURN && num != SYS_EXIT) {
        if (__entry_pos < trace_next_seq) {
            trace_ring[__entry_pos % STRACE_RING].ret = (int64_t)regs->rax;
        }
    }

    /* The interrupt path delivers pending signals from isr_handler's
       tail, but SYSCALL bypasses that. Hook it in here so a signal
       queued by e.g. self-kill is picked up on the same hop back to
       user space. SYS_SIGRETURN is the one case we must skip — regs
       has just been reloaded from sig_saved_regs and redelivering
       would clobber it. */
    if (num != SYS_SIGRETURN) process_deliver_pending_signals(regs);
    return regs;
}

/* SYSCALL/SYSRET-class MSRs. Values are the CPU-fixed numbers. */
#define MSR_EFER            0xC0000080ULL
#define MSR_STAR            0xC0000081ULL
#define MSR_LSTAR           0xC0000082ULL
#define MSR_CSTAR           0xC0000083ULL
#define MSR_FMASK           0xC0000084ULL
#define EFER_SCE            (1ULL << 0)

extern void syscall_entry_64(void);
extern uint64_t syscall_kernel_rsp;   /* from syscall_entry.s */

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static inline void wrmsr(uint32_t msr, uint64_t val) {
    uint32_t lo = (uint32_t)val, hi = (uint32_t)(val >> 32);
    __asm__ volatile ("wrmsr" :: "a"(lo), "d"(hi), "c"(msr));
}

void syscall_set_kernel_stack(uint64_t rsp) {
    syscall_kernel_rsp = rsp;
}

void syscall_init(void) {
    /* Keep INT 0x80 registered as a debuggable fallback. Real user
       code goes through the SYSCALL MSRs below. */
    isr_register_handler(0x80, syscall_handler);

    /* Enable the SYSCALL/SYSRET instruction pair via EFER.SCE. */
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);

    /* STAR layout matched to gdt.c:
         [47:32] = 0x08 → kernel CS; kernel SS is implicit at CS+8 (0x10).
         [63:48] = 0x18 → on sysretq CPU sets CS = 0x18+16 | 3 = 0x2B,
                            SS = 0x18+8 | 3 = 0x23.
       Low 32 bits are unused in long mode. */
    wrmsr(MSR_STAR, (0x08ULL << 32) | (0x18ULL << 48));

    /* Handler RIP. */
    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)&syscall_entry_64);

    /* Clear IF,DF,TF on syscall entry — keeps interrupts off while
       the entry stub swaps stacks, strips direction-flag state. */
    wrmsr(MSR_FMASK, 0x200 | 0x400 | 0x100);

    /* CSTAR is the compat-mode entry; we don't support it. Point at
       a safe stub (same as LSTAR) so a stray sysenter from compat
       mode doesn't dispatch through garbage. */
    wrmsr(MSR_CSTAR, (uint64_t)(uintptr_t)&syscall_entry_64);

    /* Initial kernel stack (the boot stack). task_init/schedule will
       override via syscall_set_kernel_stack once processes exist. */
    extern uint8_t stack_top[];
    syscall_kernel_rsp = (uint64_t)(uintptr_t)stack_top;
}
