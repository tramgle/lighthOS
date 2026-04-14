#include "kernel/syscall.h"
#include "kernel/isr.h"
#include "kernel/task.h"
#include "kernel/process.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "fs/vfs.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "mm/heap.h"
#include "include/io.h"
#include "kernel/timer.h"
#include "fs/blkdev.h"

/* --- Convenience wrappers for user-pointer validation ---
   USER_PATH copies a NUL-terminated path from user space into a
   kernel-local char array. On fault/overlong, bails out with eax=-1.
   USER_BUF_OK / USER_STRUCT_OK return early with eax=-1 on bad ptrs.
   These keep the case bodies readable and uniform. */
#define USER_PATH(user_ptr_reg, kbuf_name)                                    \
    char kbuf_name[VFS_MAX_PATH];                                             \
    if (strncpy_from_user(kbuf_name, (const char *)(user_ptr_reg),            \
                          sizeof kbuf_name) < 0) {                            \
        regs->eax = (uint32_t)-1; return regs;                                \
    }
#define USER_BUF_OK(ptr, len, write)                                          \
    do {                                                                      \
        if (!user_ptr_ok((const void *)(ptr), (len), (write))) {              \
            regs->eax = (uint32_t)-1; return regs;                            \
        }                                                                     \
    } while (0)
#define USER_STRUCT_OK(ptr, type, write) USER_BUF_OK(ptr, sizeof(type), write)

/* -- lsblk syscall payload. Mirrored in user/syscall.h. -- */
struct blkdev_info_out {
    char     name[32];
    uint32_t total_sectors;
    char     mount_path[32];   /* empty if not mounted */
    char     fs_type[16];
    uint32_t read_only;        /* 0 or 1 */
};

/* -- Diag syscall support -- */

struct meminfo_out {
    uint32_t total_frames;
    uint32_t free_frames;
    uint32_t heap_used;
    uint32_t heap_free;
};
struct region_out {
    uint32_t start_addr;
    uint32_t end_addr;
    uint32_t used;
};
struct pagemap_out {
    uint32_t pde;
    uint32_t pte;
    uint32_t phys;
    uint32_t pd_idx;
    uint32_t pt_idx;
};

/* Two-pass region iteration: the first pass counts runs until we hit the
   requested index; the second pass returns that run. pmm_region_iter
   doesn't have a "return nth" variant, so we piggyback on the callback
   via file-locals. Syscalls are serialized (single-CPU, int-gate) so
   sharing these across calls is safe. */
static uint32_t regions_target;
static uint32_t regions_seen;
static struct region_out regions_result;
static bool regions_found;

static void regions_collect(uint32_t start_frame, uint32_t len, bool used) {
    if (regions_found) return;
    if (regions_seen == regions_target) {
        regions_result.start_addr = start_frame * PAGE_SIZE;
        regions_result.end_addr = (start_frame + len) * PAGE_SIZE;
        regions_result.used = used ? 1 : 0;
        regions_found = true;
    }
    regions_seen++;
}

static registers_t *syscall_handler(registers_t *regs) {
    uint32_t num = regs->eax;

    switch (num) {
    case SYS_EXIT: {
        process_t *p = process_current();
        if (p) {
            process_exit((int)regs->ebx);
        } else {
            task_current()->state = TASK_DEAD;
        }
        return schedule(regs);
    }

    case SYS_READ: {
        int fd = (int)regs->ebx;
        size_t count = regs->edx;
        if (count > 0) USER_BUF_OK(regs->ecx, count, 1);
        regs->eax = (uint32_t)fd_read(fd, (void *)regs->ecx, count);
        return regs;
    }

    case SYS_WRITE: {
        int fd = (int)regs->ebx;
        size_t count = regs->edx;
        if (count > 0) USER_BUF_OK(regs->ecx, count, 0);
        regs->eax = (uint32_t)fd_write(fd, (const void *)regs->ecx, count);
        return regs;
    }

    case SYS_OPEN: {
        USER_PATH(regs->ebx, upath);
        char rp[VFS_MAX_PATH];
        if (process_resolve_path(upath, rp, sizeof rp) != 0) {
            regs->eax = (uint32_t)-1; return regs;
        }
        regs->eax = (uint32_t)fd_open(rp, regs->ecx);
        return regs;
    }

    case SYS_CLOSE: {
        regs->eax = (uint32_t)fd_close((int)regs->ebx);
        return regs;
    }

    case SYS_DUP2: {
        int oldfd = (int)regs->ebx;
        int newfd = (int)regs->ecx;
        regs->eax = (uint32_t)fd_dup2(oldfd, newfd);
        return regs;
    }

    case SYS_STAT: {
        USER_PATH(regs->ebx, upath);
        USER_STRUCT_OK(regs->ecx, struct vfs_stat, 1);
        char rp[VFS_MAX_PATH];
        if (process_resolve_path(upath, rp, sizeof rp) != 0) {
            regs->eax = (uint32_t)-1; return regs;
        }
        /* Stat into a kernel scratch, then copy out — keeps the VFS
           ignorant of user-pointer concerns. */
        struct vfs_stat ks;
        int32_t rc = vfs_stat(rp, &ks);
        if (rc == 0) copy_to_user((void *)regs->ecx, &ks, sizeof ks);
        regs->eax = (uint32_t)rc;
        return regs;
    }

    case SYS_UNLINK: {
        USER_PATH(regs->ebx, upath);
        char rp[VFS_MAX_PATH];
        if (process_resolve_path(upath, rp, sizeof rp) != 0) {
            regs->eax = (uint32_t)-1; return regs;
        }
        regs->eax = (uint32_t)vfs_unlink(rp);
        return regs;
    }

    case SYS_MKDIR: {
        USER_PATH(regs->ebx, upath);
        char rp[VFS_MAX_PATH];
        if (process_resolve_path(upath, rp, sizeof rp) != 0) {
            regs->eax = (uint32_t)-1; return regs;
        }
        regs->eax = (uint32_t)vfs_mkdir(rp);
        return regs;
    }

    case SYS_GETPID: {
        process_t *p = process_current();
        regs->eax = p ? p->pid : task_current()->id;
        return regs;
    }

    case SYS_YIELD:
        return schedule(regs);

    case SYS_WAITPID: {
        if (regs->ecx) USER_STRUCT_OK(regs->ecx, int, 1);
        int status = 0;
        regs->eax = (uint32_t)process_waitpid(regs->ebx, &status);
        if (regs->ecx) *(int *)regs->ecx = status;
        return regs;
    }

    case SYS_SBRK: {
        process_t *p = process_current();
        if (!p) { regs->eax = (uint32_t)-1; return regs; }
        uint32_t increment = regs->ebx;
        uint32_t old_brk = p->brk;
        if (increment == 0) {
            regs->eax = old_brk;
            return regs;
        }
        /* Allocate pages for the new break region */
        uint32_t new_brk = old_brk + increment;
        uint32_t page = old_brk & ~(PAGE_SIZE - 1);
        while (page < new_brk) {
            if (!vmm_get_physical(page)) {
                uint32_t frame = pmm_alloc_frame();
                if (!frame) { regs->eax = (uint32_t)-1; return regs; }
                vmm_map_page(page, frame, VMM_FLAG_PRESENT | VMM_FLAG_WRITE | VMM_FLAG_USER);
            }
            page += PAGE_SIZE;
        }
        p->brk = new_brk;
        regs->eax = old_brk;
        return regs;
    }

    case SYS_READDIR: {
        USER_PATH(regs->ebx, upath);
        /* name output buffer: conservative VFS_MAX_NAME bytes; type out: uint32_t. */
        USER_BUF_OK(regs->edx, VFS_MAX_NAME, 1);
        USER_STRUCT_OK(regs->esi, uint32_t, 1);
        char rp[VFS_MAX_PATH];
        if (process_resolve_path(upath, rp, sizeof rp) != 0) {
            regs->eax = (uint32_t)-1; return regs;
        }
        char kname[VFS_MAX_NAME];
        uint32_t ktype = 0;
        int32_t rc = vfs_readdir(rp, regs->ecx, kname, &ktype);
        if (rc == 0) {
            copy_to_user((void *)regs->edx, kname, VFS_MAX_NAME);
            *(uint32_t *)regs->esi = ktype;
        }
        regs->eax = (uint32_t)rc;
        return regs;
    }

    case SYS_CHDIR: {
        USER_PATH(regs->ebx, upath);
        char rp[VFS_MAX_PATH];
        if (process_resolve_path(upath, rp, sizeof rp) != 0) {
            regs->eax = (uint32_t)-1; return regs;
        }
        struct vfs_stat st;
        if (vfs_stat(rp, &st) != 0 || st.type != VFS_DIR) {
            regs->eax = (uint32_t)-1;
        } else {
            process_t *p = process_current();
            if (p) strncpy(p->cwd, rp, VFS_MAX_PATH - 1);
            regs->eax = 0;
        }
        return regs;
    }

    case SYS_GETCWD: {
        size_t size = regs->ecx;
        if (!regs->ebx || size == 0) { regs->eax = 0; return regs; }
        USER_BUF_OK(regs->ebx, size, 1);
        process_t *p = process_current();
        if (!p) { regs->eax = 0; return regs; }
        /* Build into kernel buffer then copy back. */
        char kbuf[VFS_MAX_PATH];
        strncpy(kbuf, p->cwd, sizeof kbuf - 1);
        kbuf[sizeof kbuf - 1] = '\0';
        size_t klen = strlen(kbuf);
        if (klen >= size) klen = size - 1;
        copy_to_user((void *)regs->ebx, kbuf, klen);
        char nul = '\0';
        copy_to_user((void *)(regs->ebx + klen), &nul, 1);
        regs->eax = regs->ebx;
        return regs;
    }

    case SYS_FORK: {
        regs->eax = (uint32_t)process_fork(regs);
        return regs;
    }

    case SYS_EXECVE: {
        USER_PATH(regs->ebx, upath);
        /* argv is validated inside process_execve's snapshot loop
           — it walks char *const * and calls strlen on each entry.
           That loop should itself use the user-safe helpers, but for
           now we pre-validate the argv pointer block lightly: a NULL
           argv is fine; non-NULL must be in user space. */
        if (regs->ecx) USER_STRUCT_OK(regs->ecx, void *, 0);
        char rp[VFS_MAX_PATH];
        if (process_resolve_path(upath, rp, sizeof rp) != 0) {
            regs->eax = (uint32_t)-1; return regs;
        }
        int rc = process_execve(regs, rp, (char *const *)regs->ecx);
        if (rc != 0) regs->eax = (uint32_t)-1;
        return regs;
    }

    case SYS_SPAWN: {
        USER_PATH(regs->ebx, upath);
        if (regs->ecx) USER_STRUCT_OK(regs->ecx, void *, 0);
        char rp[VFS_MAX_PATH];
        if (process_resolve_path(upath, rp, sizeof rp) != 0) {
            regs->eax = (uint32_t)-1; return regs;
        }
        regs->eax = (uint32_t)process_spawn(rp, (char *const *)regs->ecx);
        return regs;
    }

    case SYS_PS: {
        USER_STRUCT_OK(regs->ecx, proc_info_t, 1);
        proc_info_t kout;
        int32_t rc = process_info(regs->ebx, &kout);
        if (rc == 0) copy_to_user((void *)regs->ecx, &kout, sizeof kout);
        regs->eax = (uint32_t)rc;
        return regs;
    }

    case SYS_MEMINFO: {
        USER_STRUCT_OK(regs->ebx, struct meminfo_out, 1);
        struct meminfo_out k;
        k.total_frames = pmm_get_total_count();
        k.free_frames  = pmm_get_free_count();
        k.heap_used    = heap_get_used();
        k.heap_free    = heap_get_free();
        copy_to_user((void *)regs->ebx, &k, sizeof k);
        regs->eax = 0;
        return regs;
    }

    case SYS_REGIONS: {
        USER_STRUCT_OK(regs->ecx, struct region_out, 1);
        regions_target = regs->ebx;
        regions_seen = 0;
        regions_found = false;
        pmm_region_iter(regions_collect);
        if (regions_found) {
            copy_to_user((void *)regs->ecx, &regions_result, sizeof regions_result);
            regs->eax = 0;
        } else {
            regs->eax = (uint32_t)-1;
        }
        return regs;
    }

    case SYS_PAGEMAP: {
        USER_STRUCT_OK(regs->ecx, struct pagemap_out, 1);
        uint32_t va = regs->ebx;
        uint32_t *pd = task_current_pd();
        if (!pd) { regs->eax = (uint32_t)-1; return regs; }
        struct pagemap_out k = {0};
        k.pd_idx = va >> 22;
        k.pt_idx = (va >> 12) & 0x3FF;
        k.pde = pd[k.pd_idx];
        if (k.pde & VMM_FLAG_PRESENT) {
            uint32_t *pt = (uint32_t *)(k.pde & 0xFFFFF000);
            k.pte = pt[k.pt_idx];
            if (k.pte & VMM_FLAG_PRESENT) {
                k.phys = (k.pte & 0xFFFFF000) | (va & 0xFFF);
            }
        }
        copy_to_user((void *)regs->ecx, &k, sizeof k);
        regs->eax = 0;
        return regs;
    }

    case SYS_PEEK: {
        /* Copy `count` bytes from arbitrary kernel address `src` into the
           user's buffer. Restrict reads to the first 16MB identity-mapped
           region on the source side, and validate the user destination
           range. */
        uint32_t src = regs->ebx;
        uint32_t count = regs->edx;
        if (count == 0) { regs->eax = 0; return regs; }
        if (src >= 0x01000000u || src + count > 0x01000000u ||
            src + count < src) {
            regs->eax = (uint32_t)-1;
            return regs;
        }
        if (copy_to_user((void *)regs->ecx, (const void *)src, count) < 0) {
            regs->eax = (uint32_t)-1; return regs;
        }
        regs->eax = (uint32_t)count;
        return regs;
    }

    case SYS_TIME: {
        /* Ticks since boot at 100Hz. User code divides by 100 for seconds. */
        regs->eax = timer_get_ticks();
        return regs;
    }

    case SYS_BLKDEVS: {
        USER_STRUCT_OK(regs->ecx, struct blkdev_info_out, 1);
        blkdev_t *dev = blkdev_nth((int)regs->ebx);
        if (!dev) { regs->eax = (uint32_t)-1; return regs; }
        struct blkdev_info_out k;
        memset(&k, 0, sizeof k);
        strncpy(k.name, dev->name, sizeof k.name - 1);
        k.total_sectors = dev->total_sectors;
        strncpy(k.mount_path, dev->mount_path, sizeof k.mount_path - 1);
        strncpy(k.fs_type, dev->fs_type, sizeof k.fs_type - 1);
        k.read_only = dev->read_only ? 1 : 0;
        copy_to_user((void *)regs->ecx, &k, sizeof k);
        regs->eax = 0;
        return regs;
    }

    case SYS_LSEEK: {
        int fd = (int)regs->ebx;
        off_t off = (off_t)(int32_t)regs->ecx;
        int whence = (int)regs->edx;
        regs->eax = (uint32_t)fd_lseek(fd, off, whence);
        return regs;
    }

    case SYS_SHUTDOWN: {
        __asm__ volatile ("cli");
        /* QEMU ACPI shutdown (i440fx/PIIX4, and older Bochs fallback) */
        outw(0x604, 0x2000);
        outw(0xB004, 0x2000);
        for (;;) __asm__ volatile ("hlt");
    }

    default:
        serial_printf("[syscall] unknown syscall %u\n", num);
        regs->eax = (uint32_t)-1;
        return regs;
    }
}

void syscall_init(void) {
    isr_register_handler(0x80, syscall_handler);
    serial_printf("[syscall] Handler registered at int 0x80\n");
}
