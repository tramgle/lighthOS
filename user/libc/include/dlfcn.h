#ifndef _DLFCN_H
#define _DLFCN_H

/* POSIX dlfcn shim over LighthOS's ld-lighthos.so.1 published ops
   table at 0x50000000. Only dynamic binaries can use these — a
   static binary has no ld.so running and the ops table isn't
   mapped. Flags are accepted but currently ignored; our runtime
   linker is effectively always RTLD_NOW | RTLD_GLOBAL. */

#define RTLD_LAZY   0x0001
#define RTLD_NOW    0x0002
#define RTLD_LOCAL  0x0000
#define RTLD_GLOBAL 0x0100

void       *dlopen(const char *path, int flags);
void       *dlsym(void *handle, const char *name);
int         dlclose(void *handle);
const char *dlerror(void);

#endif
