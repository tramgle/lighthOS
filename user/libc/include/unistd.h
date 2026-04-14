#ifndef _UNISTD_H
#define _UNISTD_H

#include <stddef.h>

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

int   close(int fd);
long  lseek(int fd, long off, int whence);
int   read(int fd, void *buf, size_t n);
int   write(int fd, const void *buf, size_t n);
int   isatty(int fd);
int   unlink(const char *path);

#endif
