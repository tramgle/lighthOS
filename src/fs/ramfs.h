#ifndef RAMFS_H
#define RAMFS_H

#include "fs/vfs.h"

vfs_ops_t  *ramfs_get_ops(void);
vfs_node_t *ramfs_init(void);

#endif
