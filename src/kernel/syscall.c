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
#include "mm/vmm.h"
#include "mm/heap.h"
#include "drivers/serial.h"

/* Thin wrappers so the SYS_EXECVE case body reads cleanly even
   though heap.h's names don't have module-prefix namespacing. */
static inline void *kmalloc_wrap(uint64_t n) { return kmalloc(n); }
static inline void  kfree_wrap(void *p)      { kfree(p); }

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
#define SYS_PIPE    42
#define SYS_DUP2    63
#define SYS_FORK    57
#define SYS_EXECVE  59
#define SYS_READDIR 89
#define SYS_SPAWN  120
#define SYS_SHUTDOWN 201
#define SYS_TIME    214

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
    (void)a3;

    switch (num) {
    case SYS_EXIT:
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

    case SYS_OPEN:
        regs->rax = (uint64_t)(int64_t)fd_open(
            (const char *)(uintptr_t)a1, (uint32_t)a2);
        break;

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

    case SYS_UNLINK:
        regs->rax = (uint64_t)(int64_t)vfs_unlink((const char *)(uintptr_t)a1);
        break;

    case SYS_STAT: {
        struct vfs_stat kst;
        int r = vfs_stat((const char *)(uintptr_t)a1, &kst);
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

    case SYS_MKDIR:
        regs->rax = (uint64_t)(int64_t)vfs_mkdir((const char *)(uintptr_t)a1);
        break;

    case SYS_DUP2:
        regs->rax = (uint64_t)(int64_t)fd_dup2((int)a1, (int)a2);
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
        const char *path = (const char *)(uintptr_t)a1;
        char *const *argv = (char *const *)(uintptr_t)a2;
        struct vfs_stat st;
        if (vfs_stat(path, &st) != 0 || st.size == 0) {
            regs->rax = (uint64_t)(int64_t)-1; break;
        }
        void *buf = kmalloc_wrap(st.size);
        if (!buf) { regs->rax = (uint64_t)(int64_t)-1; break; }
        ssize_t r = vfs_read(path, buf, st.size, 0);
        if (r != (ssize_t)st.size) {
            kfree_wrap(buf);
            regs->rax = (uint64_t)(int64_t)-1; break;
        }
        int rc = process_execve_from_memory(regs, path, buf, st.size, argv);
        kfree_wrap(buf);
        if (rc < 0) regs->rax = (uint64_t)(int64_t)-1;
        /* Success: regs is already rewritten — when we return, the
           stub iretq's into the new image at entry with the fresh
           user stack we built. */
        break;
    }

    case SYS_READDIR: {
        char name[VFS_MAX_NAME];
        uint32_t type = 0;
        int r = vfs_readdir((const char *)(uintptr_t)a1,
                            (uint32_t)a2, name, &type);
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

    default:
        kprintf("[syscall] unknown %lu from pid %u\n", num,
                process_current() ? process_current()->pid : 0);
        regs->rax = (uint64_t)(int64_t)-1;
        break;
    }
    return regs;
}

void syscall_init(void) {
    isr_register_handler(0x80, syscall_handler);
}
