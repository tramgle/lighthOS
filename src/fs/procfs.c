/* procfs — synthetic read-only filesystem that surfaces kernel state.
 *
 * Layout:
 *   /proc/meminfo         MemTotal, MemFree (kB)
 *   /proc/mounts          one line per registered blkdev mount
 *   /proc/<pid>/status    Name, Pid, PPid, Pgid, State
 *   /proc/<pid>/cmdline   NUL-separated argv
 *
 * Nodes are lazy: every finddir() kmalloc's a fresh vfs_node_t +
 * proc_node_t (same leak profile as ramfs/fat). read() composes the
 * answer from live data per call — no storage, no snapshotting. This
 * keeps the PID-exit race trivial: a stale node whose pid has since
 * been reaped simply reads as empty. */

#include "fs/procfs.h"
#include "fs/blkdev.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "lib/string.h"
#include "kernel/process.h"
#include "kernel/task.h"

enum proc_kind {
    PROC_ROOT,
    PROC_MEMINFO,
    PROC_MOUNTS,
    PROC_PID_DIR,
    PROC_PID_STATUS,
    PROC_PID_CMDLINE,
};

typedef struct {
    enum proc_kind kind;
    uint32_t       pid;
} proc_node_t;

static vfs_ops_t procfs_ops;

/* ---- tiny formatting helpers (no printf in kernel for this path) ---- */

static int utoa_dec(uint64_t v, char *out) {
    char tmp[24];
    int i = 0;
    if (v == 0) tmp[i++] = '0';
    while (v > 0) { tmp[i++] = (char)('0' + (v % 10)); v /= 10; }
    int j = 0;
    while (i > 0) out[j++] = tmp[--i];
    return j;
}

static int append_str(char *dst, int cap, int pos, const char *s) {
    while (*s && pos < cap - 1) dst[pos++] = *s++;
    return pos;
}

static int append_u(char *dst, int cap, int pos, uint64_t v) {
    char buf[24];
    int n = utoa_dec(v, buf);
    for (int i = 0; i < n && pos < cap - 1; i++) dst[pos++] = buf[i];
    return pos;
}

/* ---- allocation + node plumbing ---- */

static vfs_node_t *new_node(const char *name, uint32_t type,
                            enum proc_kind kind, uint32_t pid) {
    proc_node_t *pn = kmalloc(sizeof(proc_node_t));
    if (!pn) return 0;
    pn->kind = kind;
    pn->pid  = pid;

    vfs_node_t *v = kmalloc(sizeof(vfs_node_t));
    if (!v) { kfree(pn); return 0; }
    memset(v, 0, sizeof(*v));
    strncpy(v->name, name, VFS_MAX_NAME - 1);
    v->type         = type;
    v->ops          = &procfs_ops;
    v->private_data = pn;
    return v;
}

/* Parse an unsigned-decimal pid string. Rejects the empty string,
 * non-digits, and anything that would overflow uint32_t — otherwise a
 * pathological "/proc/99999999999" request would wrap silently and
 * hit an unrelated live pid. */
static int is_all_digits(const char *s, uint32_t *val_out) {
    if (!s || !*s) return 0;
    uint64_t v = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return 0;
        v = v * 10 + (uint64_t)(*p - '0');
        if (v > 0xFFFFFFFFu) return 0;
    }
    *val_out = (uint32_t)v;
    return 1;
}

/* ---- content composers ---- */

static int compose_meminfo(char *out, int cap) {
    int p = 0;
    uint64_t total_kb = (uint64_t)pmm_get_total_count() * 4u;
    uint64_t free_kb  = (uint64_t)pmm_get_free_count()  * 4u;
    p = append_str(out, cap, p, "MemTotal: ");
    p = append_u  (out, cap, p, total_kb);
    p = append_str(out, cap, p, " kB\nMemFree: ");
    p = append_u  (out, cap, p, free_kb);
    p = append_str(out, cap, p, " kB\n");
    return p;
}

static int compose_mounts(char *out, int cap) {
    int p = 0;
    int n = blkdev_count();
    for (int i = 0; i < n; i++) {
        blkdev_t *d = blkdev_nth(i);
        if (!d || d->mount_path[0] == '\0') continue;
        p = append_str(out, cap, p, d->name);
        p = append_str(out, cap, p, " ");
        p = append_str(out, cap, p, d->mount_path);
        p = append_str(out, cap, p, " ");
        p = append_str(out, cap, p,
                       d->fs_type[0] ? d->fs_type : "unknown");
        p = append_str(out, cap, p, d->read_only ? " ro\n" : " rw\n");
    }
    return p;
}

static char task_state_char(uint32_t state) {
    /* Matches task_state_t: READY=0, RUNNING=1, BLOCKED=2, STOPPED=3, DEAD=4 */
    switch (state) {
    case 0: return 'R';   /* ready */
    case 1: return 'R';   /* running */
    case 2: return 'S';   /* blocked → treat as "sleeping" */
    case 3: return 'T';   /* stopped */
    case 4: return 'Z';   /* dead — between exit and reap */
    default: return '?';
    }
}

static int compose_pid_status(uint32_t pid, char *out, int cap) {
    process_t *proc = process_get(pid);
    if (!proc) return 0;
    int p = 0;
    p = append_str(out, cap, p, "Name: ");
    p = append_str(out, cap, p, proc->name);
    p = append_str(out, cap, p, "\nPid: ");
    p = append_u  (out, cap, p, proc->pid);
    p = append_str(out, cap, p, "\nPPid: ");
    p = append_u  (out, cap, p, proc->parent_pid);
    p = append_str(out, cap, p, "\nPgid: ");
    p = append_u  (out, cap, p, proc->pgid);
    p = append_str(out, cap, p, "\nState: ");
    char st[2] = { task_state_char(proc->task ? (uint32_t)proc->task->state : 4u),
                   '\0' };
    p = append_str(out, cap, p, st);
    p = append_str(out, cap, p, "\nUTime: ");
    p = append_u  (out, cap, p, proc->utime_ticks);
    p = append_str(out, cap, p, "\nSTime: ");
    p = append_u  (out, cap, p, proc->stime_ticks);
    p = append_str(out, cap, p, "\nStartTicks: ");
    p = append_u  (out, cap, p, proc->start_ticks);
    p = append_str(out, cap, p, "\n");
    return p;
}

/* Append `max_len` bytes or up to the first NUL from `src` to `dst`,
 * whichever comes first — same spirit as strnlen-before-strcpy. Used
 * below to keep a corrupted or non-terminated spawn_argv_buf entry
 * from walking past the buffer. */
static int append_bounded(char *dst, int cap, int pos,
                          const char *src, int max_len) {
    for (int i = 0; i < max_len && src[i] && pos < cap - 1; i++) {
        dst[pos++] = src[i];
    }
    return pos;
}

static int compose_pid_cmdline(uint32_t pid, char *out, int cap) {
    process_t *proc = process_get(pid);
    if (!proc) return 0;
    /* NUL-separated argv from the spawn trampoline buffer. If this
       process exec'd without argv forwarding (fork() without a
       following execve), spawn_argc may be zero — fall back to the
       binary name in that case. */
    int p = 0;
    int argc = proc->spawn_argc;
    if (argc <= 0) {
        p = append_str(out, cap, p, proc->name);
        if (p < cap) out[p++] = '\0';
        return p;
    }
    for (int i = 0; i < argc && i < SPAWN_ARGV_MAX; i++) {
        uint64_t off = proc->spawn_argv_off[i];
        if (off >= SPAWN_ARGV_BUF) continue;
        /* Cap the per-string read at the remaining buffer — if the
           kernel's argv setup ever forgets a NUL, we still won't walk
           past spawn_argv_buf. */
        int limit = (int)(SPAWN_ARGV_BUF - off);
        p = append_bounded(out, cap, p, &proc->spawn_argv_buf[off], limit);
        if (p < cap) out[p++] = '\0';
    }
    return p;
}

/* ---- vfs_ops ---- */

static int procfs_open (vfs_node_t *n, uint32_t f) { (void)n; (void)f; return 0; }
static int procfs_close(vfs_node_t *n)             { (void)n; return 0; }

static ssize_t procfs_read(vfs_node_t *node, void *buf,
                           size_t size, off_t offset) {
    proc_node_t *pn = (proc_node_t *)node->private_data;
    if (!pn) return -1;
    if (offset < 0) return -1;

    char tmp[1024];
    int len = 0;
    switch (pn->kind) {
    case PROC_MEMINFO:     len = compose_meminfo(tmp, sizeof tmp); break;
    case PROC_MOUNTS:      len = compose_mounts (tmp, sizeof tmp); break;
    case PROC_PID_STATUS:  len = compose_pid_status (pn->pid, tmp, sizeof tmp); break;
    case PROC_PID_CMDLINE: len = compose_pid_cmdline(pn->pid, tmp, sizeof tmp); break;
    default: return -1;
    }

    /* Compare as off_t both sides so callers supplying an offset
       larger than any composer ever returns can't underflow avail. */
    if (offset >= (off_t)len) return 0;
    size_t avail = (size_t)((off_t)len - offset);
    if (size > avail) size = avail;
    memcpy(buf, tmp + offset, size);
    return (ssize_t)size;
}

/* readdir for root: static entries first, then live pids in slot
 * order. NOTE: process_info_at walks the processes[] array by live-
 * slot index, so an exit + re-allocate between two readdir calls
 * can renumber the pid sequence. Callers doing `ls /proc` under
 * churn must accept the snapshot isn't stable — same contract as
 * Linux /proc. */
static int procfs_readdir(vfs_node_t *dir, uint32_t index,
                          char *name_out, uint32_t *type_out) {
    proc_node_t *pn = (proc_node_t *)dir->private_data;
    if (!pn) return -1;

    if (pn->kind == PROC_ROOT) {
        if (index == 0) { strncpy(name_out, "meminfo", VFS_MAX_NAME); *type_out = VFS_FILE; return 0; }
        if (index == 1) { strncpy(name_out, "mounts",  VFS_MAX_NAME); *type_out = VFS_FILE; return 0; }
        uint32_t pi = index - 2;
        struct proc_info info;
        if (process_info_at(pi, &info) != 0) return -1;
        int n = utoa_dec(info.pid, name_out);
        name_out[n] = '\0';
        *type_out = VFS_DIR;
        return 0;
    }

    if (pn->kind == PROC_PID_DIR) {
        if (index == 0) { strncpy(name_out, "status",  VFS_MAX_NAME); *type_out = VFS_FILE; return 0; }
        if (index == 1) { strncpy(name_out, "cmdline", VFS_MAX_NAME); *type_out = VFS_FILE; return 0; }
        return -1;
    }

    return -1;
}

static vfs_node_t *procfs_finddir(vfs_node_t *dir, const char *name) {
    proc_node_t *pn = (proc_node_t *)dir->private_data;
    if (!pn) return 0;

    /* .. is handled textually by the VFS resolver above us; reject
       to be safe. */
    if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')))
        return 0;

    if (pn->kind == PROC_ROOT) {
        if (strcmp(name, "meminfo") == 0)
            return new_node("meminfo", VFS_FILE, PROC_MEMINFO, 0);
        if (strcmp(name, "mounts") == 0)
            return new_node("mounts",  VFS_FILE, PROC_MOUNTS, 0);
        /* /proc/self — snapshot the current process's pid into a
           PROC_PID_DIR. No real symlinks yet; this gives callers the
           same ergonomics for finding their own status/cmdline. */
        if (strcmp(name, "self") == 0) {
            process_t *cur = process_current();
            if (!cur) return 0;
            return new_node("self", VFS_DIR, PROC_PID_DIR, cur->pid);
        }
        uint32_t pid;
        if (is_all_digits(name, &pid) && process_get(pid))
            return new_node(name, VFS_DIR, PROC_PID_DIR, pid);
        return 0;
    }

    if (pn->kind == PROC_PID_DIR) {
        /* Ensure the pid still exists; otherwise ENOENT. */
        if (!process_get(pn->pid)) return 0;
        if (strcmp(name, "status") == 0)
            return new_node("status",  VFS_FILE, PROC_PID_STATUS,  pn->pid);
        if (strcmp(name, "cmdline") == 0)
            return new_node("cmdline", VFS_FILE, PROC_PID_CMDLINE, pn->pid);
        return 0;
    }

    return 0;
}

static int procfs_stat(vfs_node_t *node, struct vfs_stat *st) {
    proc_node_t *pn = (proc_node_t *)node->private_data;
    st->inode = 0;
    st->type  = node->type;
    st->size  = 0;                 /* /proc convention — read until EOF */
    (void)pn;
    return 0;
}

/* write/create/unlink/mkdir rejected — read-only FS. */
static ssize_t procfs_write(vfs_node_t *n, const void *b, size_t s, off_t o) {
    (void)n; (void)b; (void)s; (void)o; return -1;
}
static int procfs_create(vfs_node_t *d, const char *n, uint32_t t) {
    (void)d; (void)n; (void)t; return -1;
}
static int procfs_unlink(vfs_node_t *d, const char *n) {
    (void)d; (void)n; return -1;
}
static int procfs_mkdir(vfs_node_t *d, const char *n) {
    (void)d; (void)n; return -1;
}

static vfs_ops_t procfs_ops = {
    .open    = procfs_open,
    .close   = procfs_close,
    .read    = procfs_read,
    .write   = procfs_write,
    .readdir = procfs_readdir,
    .finddir = procfs_finddir,
    .create  = procfs_create,
    .unlink  = procfs_unlink,
    .stat    = procfs_stat,
    .mkdir   = procfs_mkdir,
};

vfs_ops_t *procfs_get_ops(void) { return &procfs_ops; }

vfs_node_t *procfs_init(void) {
    return new_node("/", VFS_DIR, PROC_ROOT, 0);
}
