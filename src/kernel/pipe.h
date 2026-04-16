#ifndef PIPE_H
#define PIPE_H

#include "include/types.h"

#define PIPE_BUF_SIZE 4096

/* Unidirectional byte stream between two processes. Readers and writers
   are refcounts — each fd_entry of type FD_PIPE_READ / FD_PIPE_WRITE
   contributes one. When both counts reach zero the pipe is freed. */
typedef struct pipe {
    uint8_t  buf[PIPE_BUF_SIZE];
    uint32_t read_pos;
    uint32_t write_pos;
    uint32_t count;      /* bytes currently buffered */
    uint32_t readers;
    uint32_t writers;
} pipe_t;

/* Allocate a pipe. Starts with readers=0 / writers=0 — callers must
   install two fd_entry copies (one reader, one writer) and increment
   those counters via pipe_add_reader / pipe_add_writer. Returns NULL
   on OOM. */
pipe_t *pipe_create(void);

void pipe_add_reader(pipe_t *p);
void pipe_add_writer(pipe_t *p);

/* Close one reader / writer endpoint. When readers+writers hits 0 the
   pipe is kfree'd — do not touch p after the last close. */
void pipe_close_reader(pipe_t *p);
void pipe_close_writer(pipe_t *p);

/* Blocking read: waits for data while writers>0. Returns 0 on EOF
   (writers==0 && count==0), bytes read otherwise. */
ssize_t pipe_read(pipe_t *p, void *buf, size_t n);

/* Blocking write: loops until all n bytes are delivered or readers==0.
   Returns -1 if the write didn't start at all because readers==0,
   partial count if readers closed mid-write. */
ssize_t pipe_write(pipe_t *p, const void *buf, size_t n);

#endif
