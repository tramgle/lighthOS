#ifndef FSTAB_H
#define FSTAB_H

/* Parse a fstab-style file and drive vfs_mount for each entry.
   Format (one mount per line, `#` starts a comment):
     <source> <mountpoint> <type> <flags>
   where:
     source     — registered blkdev name (e.g. "ata0p0"), or "none"
                  for sourceless filesystems (not used yet).
     mountpoint — absolute path in the VFS.
     type       — "simplefs" or "fat16" (others can be added).
     flags      — "ro" or "rw".

   Call `fstab_mount_file("/etc/fstab")` after the ramfs is up and
   block devices are registered. Returns the number of successful
   mounts; prints errors to the serial log for failures but doesn't
   abort (missing devices / unrecognized fs-types just skip). */
int fstab_mount_file(const char *path);

/* Parse fstab entries from an in-memory NUL-terminated string. Used
   by the kernel when no ramfs-side /etc/fstab is available (e.g. the
   self-contained bootdisk flow, where there are no multiboot modules
   to carry a file). Same semantics as fstab_mount_file. */
int fstab_mount_string(const char *content);

/* Apply the built-in default layout: FAT on ata0p0 mounted directly
   at '/' (rw). Replaces the initial ramfs root so the installed
   system has no /disk indirection. Called by the kernel when no
   ramfs-side /etc/fstab is available — e.g. the self-contained
   bootdisk boot, which has no multiboot modules. */
int fstab_mount_defaults(void);

/* Apply a single mount directive imperatively — same semantics as an
   fstab line but callable from SYS_MOUNT. `source` is a blkdev name
   (e.g. "ata0p0"), `type` is "simplefs"/"fat"/"fat16"/"fat32",
   `flags` is "ro" or "rw". Returns 0 on success, -1 on failure. */
int fstab_do_mount(const char *source, const char *mountpoint,
                   const char *type, const char *flags);

#endif
