# test_fs — write + stat + read + unlink round-trip on the active
# rootfs. Exits non-zero on any step that doesn't match the expected
# bytes / size.
test_fs
echo fs-ok > /scratch/tfs.out
assert test_fs.ok fs-ok /scratch/tfs.out
