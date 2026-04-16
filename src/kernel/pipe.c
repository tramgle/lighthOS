#include "kernel/pipe.h"
#include "kernel/task.h"
#include "mm/heap.h"
#include "lib/string.h"

pipe_t *pipe_create(void) {
    pipe_t *p = kmalloc(sizeof(*p));
    if (!p) return NULL;
    memset(p, 0, sizeof(*p));
    return p;
}

void pipe_add_reader(pipe_t *p) { if (p) p->readers++; }
void pipe_add_writer(pipe_t *p) { if (p) p->writers++; }

static void pipe_maybe_free(pipe_t *p) {
    if (p->readers == 0 && p->writers == 0) {
        kfree(p);
    }
}

void pipe_close_reader(pipe_t *p) {
    if (!p) return;
    if (p->readers > 0) p->readers--;
    pipe_maybe_free(p);
}

void pipe_close_writer(pipe_t *p) {
    if (!p) return;
    if (p->writers > 0) p->writers--;
    pipe_maybe_free(p);
}

ssize_t pipe_read(pipe_t *p, void *buf, size_t n) {
    if (!p || n == 0) return 0;

    /* Block cooperatively until at least one byte is available, or the
       last writer closes (EOF). Single-CPU, no wait queues — yield and
       retry. */
    while (p->count == 0 && p->writers > 0) {
        task_yield();
    }
    if (p->count == 0) return 0;

    size_t avail = p->count;
    size_t chunk = n < avail ? n : avail;
    uint8_t *dst = (uint8_t *)buf;
    for (size_t i = 0; i < chunk; i++) {
        dst[i] = p->buf[p->read_pos];
        p->read_pos = (p->read_pos + 1) % PIPE_BUF_SIZE;
    }
    p->count -= (uint32_t)chunk;
    return (ssize_t)chunk;
}

ssize_t pipe_write(pipe_t *p, const void *buf, size_t n) {
    if (!p) return -1;
    if (n == 0) return 0;
    if (p->readers == 0) return -1;

    const uint8_t *src = (const uint8_t *)buf;
    size_t done = 0;
    while (done < n) {
        while (p->count == PIPE_BUF_SIZE && p->readers > 0) {
            task_yield();
        }
        if (p->readers == 0) {
            /* Reader gone mid-write: report partial progress, or -1 if
               nothing made it through. */
            return done > 0 ? (ssize_t)done : -1;
        }
        uint32_t free_space = PIPE_BUF_SIZE - p->count;
        size_t want = n - done;
        size_t chunk = want < free_space ? want : free_space;
        for (size_t i = 0; i < chunk; i++) {
            p->buf[p->write_pos] = src[done + i];
            p->write_pos = (p->write_pos + 1) % PIPE_BUF_SIZE;
        }
        p->count += (uint32_t)chunk;
        done += chunk;
    }
    return (ssize_t)done;
}
