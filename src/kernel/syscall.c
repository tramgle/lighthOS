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
#include "mm/heap.h"
#include "lib/string.h"
#include "drivers/serial.h"

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

static registers_t *syscall_handler(registers_t *regs) {
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
        regs->rax = (uint64_t)(int64_t)process_kill((uint32_t)a1, (int)a2);
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

    case SYS_SPAWN:
        /* Path-based spawn deferred: need to read ELF from fs.
           For now a user wrapper can bundle ELF bytes via a separate
           memory-spawn syscall when that becomes useful. */
        regs->rax = (uint64_t)(int64_t)-1;
        break;

    case SYS_TIME:
        regs->rax = timer_get_ticks();
        break;

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
    return regs;
}

void syscall_init(void) {
    isr_register_handler(0x80, syscall_handler);
}
