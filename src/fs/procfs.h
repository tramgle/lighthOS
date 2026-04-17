#ifndef PROCFS_H
#define PROCFS_H

#include "fs/vfs.h"

/* Synthetic procfs: surfaces live kernel state as a read-only tree of
 *   /proc/meminfo
 *   /proc/mounts
 *   /proc/<pid>/status
 *   /proc/<pid>/cmdline
 *
 * Nodes are allocated on demand by finddir — nothing is pre-materialised
 * except the root. read() composes content per call, so results reflect
 * current state; size in stat is always 0 per the /proc convention.
 *
 * procfs_init() is the mount-time entry point used by fstab.c's
 * "proc" type dispatch. */

vfs_node_t *procfs_init(void);
vfs_ops_t  *procfs_get_ops(void);

#endif
