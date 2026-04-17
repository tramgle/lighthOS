# /proc synthetic filesystem. Reads pull live kernel state per-call
# — no storage, no snapshotting.
grep MemTotal /proc/meminfo | wc -l > /scratch/pm
assert proc.meminfo 1 /scratch/pm

cat /proc/meminfo > /scratch/mi
grep MemFree /scratch/mi | wc -l > /scratch/mf
assert proc.memfree 1 /scratch/mf

ls /proc > /scratch/plist
grep meminfo /scratch/plist | wc -l > /scratch/pls
assert proc.lsdir 1 /scratch/pls

grep mounts /scratch/plist | wc -l > /scratch/plm
assert proc.lsdir-mounts 1 /scratch/plm
