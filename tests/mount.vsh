# umount + mount the FAT partition that /etc/fstab wired at /disk;
# confirm files round-trip through the detach/reattach cycle.

# Sanity: /disk should be mounted (ISO flow from etc/fstab).
# After umount, a probe should fail; remount should bring it back.

# Before — /disk/bin exists as a directory on the FAT partition.
ls /disk > /scratch/mnt_pre
cat /scratch/mnt_pre | wc -l > /scratch/mnt_pre_lines
# There's something in /disk (at least /bin). Line count >= 1.
# We'll just check that the file is non-empty by grep'ing for 'bin'.
grep bin /scratch/mnt_pre | wc -l > /scratch/mnt_pre_bin
assert mount.pre_has_bin 1 /scratch/mnt_pre_bin

# Detach.
umount /disk

# After umount, /disk is empty (ramfs stub directory still there from
# fstab's mkdir, but nothing in it). Confirm by grep'ing for 'bin' —
# which should no longer appear.
ls /disk > /scratch/mnt_gone
grep bin /scratch/mnt_gone | wc -l > /scratch/mnt_gone_bin
assert mount.after_umount 0 /scratch/mnt_gone_bin

# Re-mount. Should restore the same view.
mount -t fat ata0p0 /disk
ls /disk > /scratch/mnt_post
grep bin /scratch/mnt_post | wc -l > /scratch/mnt_post_bin
assert mount.post_has_bin 1 /scratch/mnt_post_bin
