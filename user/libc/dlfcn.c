/* dlfcn shim. Thin trampolines over the ops table that
   ld-lighthos.so.1 publishes at DL_IFACE_ADDR=0x50000000. A static
   binary (no ld.so) cannot use these — the page isn't mapped and
   any call will fault. */

#include <dlfcn.h>

#define DL_IFACE_ADDR 0x50000000UL

struct dl_ops {
    void       *(*dlopen)(const char *, int);
    void       *(*dlsym)(void *, const char *);
    int         (*dlclose)(void *);
    const char *(*dlerror)(void);
};

static inline struct dl_ops *ops(void) {
    return (struct dl_ops *)DL_IFACE_ADDR;
}

void *dlopen(const char *path, int flags) {
    return ops()->dlopen(path, flags);
}
void *dlsym(void *handle, const char *name) {
    return ops()->dlsym(handle, name);
}
int dlclose(void *handle) {
    return ops()->dlclose(handle);
}
const char *dlerror(void) {
    return ops()->dlerror();
}
