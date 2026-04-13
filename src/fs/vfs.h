#ifndef VFS_H
#define VFS_H

#include "include/types.h"

#define VFS_FILE    0x01
#define VFS_DIR     0x02
#define VFS_MAX_NAME 64
#define VFS_MAX_PATH 256
#define MAX_MOUNTS   8

typedef struct vfs_node    vfs_node_t;
typedef struct vfs_ops     vfs_ops_t;
typedef struct vfs_mount   vfs_mount_t;

struct vfs_stat {
    uint32_t inode;
    uint32_t type;
    uint32_t size;
};

struct vfs_ops {
    int         (*open)(vfs_node_t *node, uint32_t flags);
    int         (*close)(vfs_node_t *node);
    ssize_t     (*read)(vfs_node_t *node, void *buf, size_t size, off_t offset);
    ssize_t     (*write)(vfs_node_t *node, const void *buf, size_t size, off_t offset);
    int         (*readdir)(vfs_node_t *dir, uint32_t index, char *name_out, uint32_t *type_out);
    vfs_node_t *(*finddir)(vfs_node_t *dir, const char *name);
    int         (*create)(vfs_node_t *dir, const char *name, uint32_t type);
    int         (*unlink)(vfs_node_t *dir, const char *name);
    int         (*stat)(vfs_node_t *node, struct vfs_stat *st);
    int         (*mkdir)(vfs_node_t *dir, const char *name);
};

struct vfs_node {
    char         name[VFS_MAX_NAME];
    uint32_t     type;
    uint32_t     size;
    uint32_t     inode;
    vfs_ops_t   *ops;
    vfs_mount_t *mount;
    void        *private_data;
};

struct vfs_mount {
    char         mount_point[VFS_MAX_PATH];
    vfs_node_t  *root;
    vfs_ops_t   *ops;
    void        *fs_data;
    bool         in_use;
};

void        vfs_init(void);
int         vfs_mount(const char *path, vfs_ops_t *ops, vfs_node_t *root, void *fs_data);
int         vfs_umount(const char *path);
vfs_node_t *vfs_resolve(const char *path);
ssize_t     vfs_read(const char *path, void *buf, size_t size, off_t offset);
ssize_t     vfs_write(const char *path, const void *buf, size_t size, off_t offset);
int         vfs_readdir(const char *path, uint32_t index, char *name, uint32_t *type);
int         vfs_create(const char *path, uint32_t type);
int         vfs_unlink(const char *path);
int         vfs_stat(const char *path, struct vfs_stat *st);
int         vfs_mkdir(const char *path);

#endif
