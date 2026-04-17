# Verify the kernel boot-log machinery landed /boot.log with the
# in-ring-buffer backtrace output — same plumbing panic uses to
# persist /panic.log, so a working /boot.log proves the ring and
# flush path are wired end-to-end.
grep Backtrace: /boot.log | wc -l > /scratch/bt_count
assert bootlog.backtrace-header 1 /scratch/bt_count

grep kernel_main /boot.log | wc -l > /scratch/ks_count
assert bootlog.has-ksym 1 /scratch/ks_count
